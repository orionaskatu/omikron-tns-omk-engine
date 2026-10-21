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
   **A2 as written does not hold** - see "Step 2's bloom, tried" below: the
   additive meshes are water and glass, the lights are opaque textures.
3. **B3 then A3 + A5** - material maps, reflections, characters in the
   painted light.
4. **D1, then D2 on the player character** as the pilot for the glTF route.

Steps 1-3 are about one to two months of engine work and make a convincing
remaster; step 4 is where the small remake begins, bounded by artist time,
not code.

## Step 2's bloom, tried (2026-10-20) - and why its premise was wrong

A2 assumed the game marks its light sources by drawing them ADDITIVELY
(`0x1000|0x2000`). Built on Vulkan to find out (an emitter mask in the world
target's alpha - the luminance each additive draw adds - then a quarter-size
down / blur / add), and measured:

* **The additive meshes are mostly not lights.** Ranked by set: Lahoreh 41
  (nearly all `Eau` - water), Aapkayl 22 (`AP_mirror`, `Ap01vitre` glass,
  `eclair` lighting), Sprison 16 (`Barreau` bars), jaunpur 14 (water), the two
  sex shops 14 each (`vitre`), mayerem / Jangir 10 (water, lakes, rivers).
* **The street's signs and lamps are OPAQUE textures**, not additive. On
  Anekbah's street at strength 4 the bloom moved ~5300 pixels by about one
  565 step, all on the far skyline windows; in the apartment camera, 0.
* So a bloom that follows the data is nearly a no-op, and a bloom driven by
  brightness alone (a bright-pass) would make the sky and the pale walls glow
  - a look chosen here, with nothing in the data behind it.

**Not committed.** The working code (off by default, the default frame
unchanged) is kept outside the repo: `~/Documents/omk/omk-pack/wip/
bloom-wip.patch` + `bloom.frag`. Its post-pass (down / blur / add after the
scene pass, before the readback) is reusable for tone mapping and a grade.

**The principled way to a bloom is an EMISSIVE MAP per texture** - which
texels are light (a sign's lettering, a lamp's bulb, a window) - carried by
the texture pack beside B3's material maps, authored or generated and then
curated. That is the reader's call: it decides what in the city is light.

## Step 2, as decided (2026-10-20) - EMISSIVE bloom + a GRADE per city

**The reader's decision**, after the offline previews: an EMISSIVE map per
texture drives the bloom (not the additive draws - see "Step 2's bloom,
tried" above), then one colour grade per city, the two `[Remaster]` options
of their own, off by default. Qalisar takes the LIGHTER neon grade: "the area
is intended to be dark, and the contrast with the neons is probably a part
of the visual identity of the area" (the reader).

**Every tested parameter, kept** (the reader: "save all the tested
parameters somewhere just in case"). The scripts that made the previews, the
patch of the first bloom and the comparison page are at
`~/Documents/omk/omk-pack/compare/` (`scripts/emissive.py`, `scripts/look.py`,
`index.html`); the pixels never enter the repo.

*The emissive rule* (per texture, from the shipped texels; `l` the luma
`(299R+587G+114B)/255000`, `med` its median over the texture, `vmed` the
median of `max(R,G,B)`): a texel's emissive factor `f` is the larger of

    bright  clamp((l - max(med + 0.30, 0.55)) / 0.15)
    neon    clamp((sat - 0.55) / 0.2) * clamp((max - max(vmed + 0.25, 0.5)) / 0.15)
            with sat = (max - min) / max over the texel's R, G, B

and 0 on a keyed (pure black) texel. Of 1259 textures 1112 have some. It
finds the signs' lettering, the signboards, lit windows, lamp globes and -
with the neon term, added for Qalisar - the coloured tubes; it ALSO marks
quay edges and bright facade patches in the daylight cities (Lahoreh,
Jaunpur), which is why the maps are generated and then CURATED.

*The bloom* (composited in the preview on the 565 frame): the emissive frame
(the scene drawn with each texture's colour times `f`) box-averaged to a
quarter, blurred by a Gaussian of sigma 4 quarter-texels (21 taps) twice
each way, enlarged bilinear, times 1.3, SCREEN-blended:
`1 - (1 - base) * (1 - glow)`.

*The grades* (on 0..1 channels; `l` the luma; `S(v, a)` the S-curve
`clamp(v + a * (v - 0.45) * (1 - |2v - 1|) * 1.6)`):

| grade | city | the operations, in order |
|---|---|---|
| filmic | Jaunpur | `S(v * 1.06, 0.35)` per channel; saturation 1.12 about the new luma |
| noir | Anekbah (and Kay'l's apartment) | saturation 0.82; r*0.94, g*0.99, b*1.08; `S(max(0, v - 0.02) * 1.04, 0.3)`; in the preview ALSO a vignette `1 - 0.35 * min(1, (x^2 + y^2) * 2.2)` (x, y from -0.5 to 0.5) - spatial, so NOT in a LUT |
| sunlit | Lahoreh | `hi = max(0, 1.4 l - 0.3)`: r += 0.045 hi, g += 0.022 hi, b -= 0.03 hi; `0.03 + 0.97 v`; `S(v * 1.04, 0.22)`; saturation 1.06 |
| neon2 (chosen) | Qalisar | saturation 1.15; `sh = max(0, 1 - 2.5 l)`: r += 0.012 sh, b += 0.03 sh, g -= 0.01 sh; `0.012 + 0.988 v^0.95`; `S(v * 1.03, 0.22)` |
| neon (first, too dark) | - | saturation 1.18; the same shadow tint; `S(max(0, v - 0.015) * 1.05, 0.38)` |
| tealorange | - | `sh = max(0, 1 - 2.2 l)`, `hi = max(0, 1.6 l - 0.4)`: r += -0.035 sh + 0.06 hi, g += 0.01 sh + 0.025 hi, b += 0.05 sh - 0.04 hi; `S(v, 0.2)` |

*What the previews showed*: with the SKY in frame (the game session, not the
set alone) Anekbah's noir reads as dusk and is right; Lahoreh's sunlit and
Jaunpur's filmic kept each city's palette; the first Qalisar grade crushed
the street under its neons, neon2 keeps it dark and readable. Views: Anekbah
`--area 0 --stand 1804,0,-6890,336`; Jaunpur `--area 1 --stand
10524,-40,10284,90`; Lahoreh `--area 64` (its street start); Qalisar `--area
101 --stand 15516,0,-10182,270` (under the neons) and `,180` (the street),
the densest cluster of its 22 `Enseigne` meshes.

**Open, decided when built**: the area -> city map for interiors (they
inherit the city first); the day/night cycle may want a day and a night grade
(Qalisar at least); the grade is off while the greyscale bank is on.

## Step 2, as built (2026-10-20) - the bloom and the city grades

**What it is.** Two `[Remaster]` options of their own, off unless named,
never turned on by `all = max`, drawn by Vulkan and GLES (the software
reference, GL1 and the 3DS refuse them):

* `bloom = S` / `--bloom S` - a glow from the texture pack's LIGHT MAPS
  (`<key>_e.png`, greyscale: how much of each texel is light). 1.3 is the
  previews' strength.
* `grade = DIR` / `--grade DIR` - a colour grade per city: DIR's
  `<SET>.cube` files and `map.txt` (`AAPKAYL = ANEKBAH`), chosen each frame
  for the set under the player's feet (`PlayState::worldSet`), OFF while the
  greyscale bank is on; a hard switch, the game's own fade covers a change of
  set. The scene viewer (`--scene`) takes neither yet.

```bash
P=~/Documents/omk/omk-pack
python3 tools/omktexpack.py emissive $P/full/pack     # the light maps, ~2.5 min - then CURATE
python3 tools/omktexpack.py grades   $P/grades        # the four city .cube files + map.txt
engine/build/omk-play ../fr ../tables --save ../traces/save-appart.bin --area 101 \
    --texture-pack $P/full/pack --bloom 1.3 --grade $P/grades
```

**The preset** (the reader's "could you add a remaster arg?", 2026-10-20):
`--remaster DIR` / `[Remaster] dir = DIR` turns on all of it from one
folder - the texture pack in `DIR/pack` (or `DIR/full/pack`, which
`omktexpack.py export DIR/full` makes), the bloom at 1.3, the grades in
`DIR/grades`, the water at 0.6 (0.35 until the Fresnel). A BASE, as `all = max` is for `[Enhancements]`: `--bloom 0`,
`--texture-pack`, `--grade` or their keys beat it. With the reader's folder:

```bash
engine/build/omk-play ../fr ../tables --remaster ~/Documents/omk/omk-pack
```

`engine: remaster preset` (shown to fail with the preset's bloom dropped).

**How.** With the bloom on, the scene shader writes each texel's light into
the world target's ALPHA (Vulkan: push field `bloomMode` at 116, the
block 116 -> 120 bytes, and binding 1 of every texture's set - its light map
or a shared black one; GLES: a BLOOM VARIANT of the scene programs made from
their source at `init`, the light maps on unit 3, so the default programs and
the Vita's shader cache are untouched). After the scene, `post.frag` /
`kPostFrag`: the frame copied aside, `rgb * alpha` down to a quarter, two
Gaussian passes (21 taps, sigma 4), then one pass back into the target - the
glow SCREEN-blended, then the LUT (an N*N x N strip, blue the slice, red
fastest, trilinear). `o3de/colourgrade.*` reads `.cube` (3D, 2..64, domain
0..1) and maps sets to cities.

**Measured.** With neither on, every frame tried is byte-identical to the
build before: `run_vulkan` on Aapkayl and Qalisar under nearest, trilinear
+ 16x and 4x MSAA, and the 45-frame street dump on Vulkan and GLES, with and
without the full pack (its light maps present). The engine's Qalisar grade
against `omktexpack.grade_rgb` on the same frame: within the 565 step (mean
1.4-3.9 of 255, worst 9.4). Vulkan against GLES with both on, Qalisar under
the neons: 180 of 960000 pixels apart by more than ~16 levels. Checks:
`engine: bloom and grade` (new, shown to fail three ways), and
`engine: texture pack`, the GPU and enhancement neighbours, the Vita and 3DS
builds green.

**Open:** curating the 1112 light maps (the daylight cities' quays and pale
facades - "what counts as light" on the comparison page); a day and a night
grade (Qalisar at least, the clock runs); the scene viewer; the noir
preview's vignette is not in the LUT (spatial) - a separate option if wanted;
the Vita's shader cache does not hold the bloom variants and the post
program, so a console without the compiler refuses them.

## Step 3, A3 - water reflections: the design (2026-10-20, the reader's "ok, go")

**Why water first.** The sets are lit by BAKED vertex colour; only the
characters take per-pixel light (enhancement row 7). Normal or roughness maps
on a set would change nothing unless something relit it - which is row 8's
objection. The data does mark water: the additive meshes are mostly water and
glass (see "Step 2's bloom, tried").

**The preview that failed** (kept: `~/Documents/omk/omk-pack/wip/
water-mirror-preview.patch`): marking Jaunpur's 8 water meshes (by name, at
the commonest height) as the set's MIRROR (flag `0x100000`) and running the
existing mirror pass. The sky and half the town VANISHED and no reflection
showed on the water. The original's mirror is confined by DRAW ORDER - the
reflected pass first, the real pass over it, nothing cleared - which works
only because a mirror is an OPENING with nothing real behind it. Water has the
canal bed and the walls behind and around it, so it needs a pass of its own.

**The pass**:
1. the scene through the camera reflected in the water plane, CLIPPED to what
   is above it, sky included, into an offscreen target of its own;
2. the main pass as now; the water meshes keep their additive blend and add
   the reflection sampled at the same screen position, times a strength (a
   Fresnel term once judged);
3. a renderer entry beside `drawMirrorScene` (Vulkan: the scene pass able to
   target another framebuffer, one binding more; GLES: an FBO and a variant);
4. the game picks the water plane per set as it picks the mirror, and draws
   through the new path when the option is on and the camera is above it;
5. what counts as water: horizontal ADDITIVE surfaces - the data, not names.

Costs a second scene pass while water is in view. The vertical glass (`vitre`)
would be a further step, one plane per window.

### A3, as built (2026-10-20)

`[Remaster] water = S` / `--water S` (0..1), and `--remaster DIR` turns it
on at **0.35** (**0.6** since the Fresnel); off unless named, never `all = max`, Vulkan and GLES.

* **The plane** (`o3de/renderer.h` `waterPlane`) is the set's own data: the
  height carrying the most area of horizontal additive faces, a plane past
  1e6 square units. 7 sets have one: Lahoreh (y 12, 24.8M units, 3580
  triangles), mayerem (1072), jaunpur (4, 9.2M, 1250), PAstar (1384 -
  **Astaroth's arena: is that water, or a magic floor that should not
  reflect? for a play test**), LMoexter, ACSvent, L_Khonsu. Anekbah, the
  apartment and Qalisar have none.
* **The pass** (`drawWithWater`): the camera above the plane; the draws split
  into the water's triangles and the rest; the eye and target mirrored, the
  image flipped vertically (a half turn of roll and a screen-X flip); the
  scene clipped to what is above the plane (posed draws - the sky, the
  bodies - whole). Vulkan: the scene's render pass into a second framebuffer
  of the same attachments, binding 2 of the texture sets, push `water` /
  `waterStrength` at 120 / 124 (the block is FULL at 128); GLES: an FBO and
  a WATER VARIANT of the scene programs (unit 4), made at init only when asked.
* **Looked at** (Jaunpur's canal facing 180, Lahoreh's street start): the
  walls, railings and the arch mirror the right way up and meet the
  waterline; at 1.0 the water washes out (the reflection is ADDED to the
  water's own additive colour), at 0.35 it reads as a sheen. Vulkan and GLES
  3363 / 3326 of 960000 pixels apart.
* **Measured**: with the water off every frame tried is byte-identical to the
  build before (Vulkan and GLES, the street and the canal with the full pack;
  `run_vulkan` with 4x MSAA and trilinear). `engine: water reflection`, shown
  to fail three ways; the reflection's ORIENTATION is judged by eye, not
  checked (see the check's docstring).
* **Found on the way and fixed first** (`30f2328e`): the PNG reader's fixed
  Huffman codes were built lazily behind a plain bool, and with a pool
  decoding on the thread pool two workers rebuilt them at once - the viewer
  HUNG in Jaunpur. A function-local static now, and the inflate bounded;
  `engine: texture pack` decodes on the pool in 20 fresh processes.

* **The reader's play test, 2026-10-20 - a swimmer's legs reflected above
  his head.** The CPU clip keeps only what is above the plane, but a POSED
  draw (a body, the sky) is handed to the reflection whole - its corners are
  at rest, the pose is the GPU's - so a swimmer's submerged half was mirrored
  up through the water. Fixed in the backends: the reflection pass DISCARDS
  per fragment below the plane (Vulkan: `water` mode 2, the plane's height in
  `waterStrength`, `vWorld` is the posed position; GLES: `uClip` / `uClipY`
  and a `vWy` varying from VERTEX variants made the same way as the fragment
  one). Measured on the canal at strength 1: the shader clip alone (the whole
  scene handed over, `OMK_WATER_NO_CPU_CLIP=1`) is 77 pixels from the CPU
  clip; with no clip at all 2861. Part of `engine: water reflection` now,
  shown to fail on each backend. **CONFIRMED IN PLAY** (the reader, 2026-10-20: "Player reflect is fixed").
* **The water near the camera mirrored too strongly** (the reader's screenshot, 2026-10-20: the quay's own reflection at a steep angle). The geometry is right - the reflection meets the quay at its waterline - but one strength everywhere is not water: it reflects ~2% looking straight down and nearly all at grazing. Done: a Fresnel term (Schlick, F0 0.02) on both backends - `strength` is now the GRAZING strength, cos theta the eye ray's height over the level plane (Vulkan: the eye in the lights UBO at 400, since the push block is full; GLES: `uEye`, the ray made in the vertex stage). On the canal at strength 1 the reflection's added light per moved pixel falls from 31.2 to 20.7 (upper half, far) and from 43.0 to 19.8 (lower half, near), so lower/upper goes 1.38 -> 0.96; `engine: water reflection` part 4 asserts it under 1.15. The reflection was fainter overall and 0.35 read too weak (the reader, in play); the preset is now **0.6** - "it seems good now" (2026-10-20) - CONFIRMED IN PLAY.
* **CORRECTED 2026-10-20: the swimming canal IS the reflecting plane.** An
  earlier bullet here said the canal the player swims in lies lower than y 4;
  `todo/swimming.md` puts its surface at ~0..4 (the quay ~-120, the bed ~120),
  which is the plane, and the reader's swimming screenshot shows it
  reflecting. The camera that "dropped below y 4" in the headless runs was
  the swimmer's UNDERWATER camera. `texpack_tool water` with
  `OMK_WATER_LEVELS=1` lists every level past 1e4 square units: Jaunpur
  holds two more, both ABOVE the canal (Y down) - 405905 at y -368 (59
  triangles) and 43750 at -120 (14, the quay's height) - and Lahoreh's 16 is
  its 12 (within the 4-unit tolerance); the other five sets have one level.
  What Jaunpur's two are (terrace pools, or horizontal GLASS - additive too)
  is unread: reflecting them waits for a look, and per-level planes are not
  built.

* **The cost at 4K, measured 2026-10-20 on the M3** (load average 4-6;
  `--res 3840x2160 --frames 300 --fps`, the canal at 10524,-40,10284,180,
  `--remaster` with the pack, bloom and grades on; steady seconds, two runs
  each, alike): **Vulkan** (`--vulkan`, the swapchain presenting) 120 fps
  (vsync) with the water off, **110** with it on (12 ms worst); **GLES** 84-87
  off, **78-80** on. The reflection pass is a second 3840x2160 scene pass while
  water is in view - well inside the 30 fps yardstick on both. (`--world-vulkan`
  is the offscreen harness with a CPU present, ~40-60 fps either way - not the
  game's path, not quoted.)

**Open**: ripples (a normal map
from B3 perturbing the lookup); PAstar; the glass (`vitre`), one plane each;
the per-level planes above (once Jaunpur's two upper levels are looked at).

## Step 3, A3 continued - the reflecting floors and the lit reflections: as built (2026-10-21)

The reader's "do 1 and 2" (the wet and polished floors; the reflections lit by
the tiled point lights).

* **The floors, planar - the water's pass**, not screen-space: it shows what
  the camera cannot (SSR reuses the frame and fails at its edges), it was
  judged good on the water, and Qalisar's street is one level. `[Remaster]
  floors = S` / `--floors S` (0..1), the `--remaster` preset's **1**, off unless
  named; Vulkan and GLES (its water variant, now built when the floors alone
  are on).
* **Which floors**: the grades folder's `floors.txt` (`SET TEXTURE =
  STRENGTH`, `o3de/floorreflect.h`), curated by eye as `lights.txt` is;
  `omktexpack.py grades` writes the draft (CITY_FLOORS) when absent, and
  `texpack_tool floors <set.3DO>` lists a set's LEVEL opaque area by texture
  and height. Qalisar's draft: its street, BATITR09 at 0 and 4 (4060 m2) and
  CATITR03 at 0 (862), at 0.5; its raised walks, CATITR02 (774), at 0.35.
* **The plane**: a set's listed level triangles grouped by height, levels
  within 8 units one plane at the HIGHEST (the rest lie below and the
  reflection clips them - at a mean height the top floor was in its own
  reflection); each frame the level below the eye with the most listed floor
  within 40 m (`chooseFloor`); the water first when its plane reflects - one
  reflection a frame (the authored mirror set aside, said in the log).
* **In the frame**: `drawWithWater` keeps a listed floor's runs on the plane
  IN THE SCENE, in order (a decal and a depth tie ride on it), each carrying
  `Draw::reflect`; the backend adds the reflection there as on the water, at
  `floors x reflect`, with the Fresnel - shader mode 3, whose lookup the
  floor's RELIEF shifts (the pack's normal map, ~1% of the frame's height;
  the tiled module). The water's mode 1 is unchanged.
* **The lit reflections** (Vulkan): the water's reflection pass bins the tiled
  point lights for ITS camera into the tiles buffer's second half and binds
  its own set 1 (`ds1Refl_`); the shader no longer skips the tiles there.
  `OMK_REFL_UNLIT=1` leaves them empty, as before - byte-identical to
  `7929d606` on Jaunpur's canal. In Qalisar with the floors, lighting the
  reflection moves 57853 pixels at 640x480.
* **Found on the way, fixed**: `binLights` mirrored X a SECOND time under
  `flipX` (the basis already negates `s`) - a reflected camera's lights went
  to the other side's tiles: the tight binning 15838 pixels from the loose
  one, 0 after. The frame's camera never sets `flipX`, so nothing drawn
  before moved.
* **Off**: `--floors 0` byte-identical to `7929d606`, 14 of 15 (software,
  Vulkan, GLES; the burner, Qalisar, the canal) - the 15th the canal under
  `--enhance-all`, its water's reflection lit (1060 pixels). `engine: floors`
  new, shown to fail two ways; `env switches` had been red since the bloom
  (raw `getenv` in `run_vulkan`, `gles_probe`, `texpack_tool`) - mended.
* **The cost, and cut** (the reader's play test at 4K, M3: Qalisar 57 fps
  without the floors reflecting, 32 with): a second full scene pass. Measured
  apart - ~4.5 ms the pass (3.4 of it CPU, the 640x480 runs say), ~5.5 the
  reflection's per-fragment tiled lights at 4x MSAA. Now: the floors'
  reflection drawn at HALF size into the image's top-left quarter (one image
  and one binding for water and floors; Vulkan's viewport and render area,
  GLES's viewport and `uReflInv`), at ONE sample (`reflPass1_`, its own
  half-size depth, the scene pipelines again at 1 sample - all or none), and
  the reflection's CPU clip remembered for the SET's geometry (`clipAbove`;
  a particle's geometry is one object rewritten every frame, and a memo of it
  drew a stale clip - 1 pixel - so the others are clipped as before). Each
  step byte-identical where it should be (the memo to the clip it replaces;
  the water's frames to `7929d606` unlit). 4K Qalisar with the floors: 32 ->
  35-36 fps (floors off 42-48 in the same runs, the machine loaded); the
  half-size and one-sample pictures are hard to tell from the full one.
* **Open**: the list (Qalisar's draft only), the strengths, the relief's
  shift - for a play test; the authored mirror's pass still plain (no tiles);
  floors and water in one frame; GLES's reflection keeps its nearest slots;
  the second pass's CPU recording (~3 ms) left as it is.
* **OPEN BUG - the reader's 4K slowdown with the floors (2026-10-21, M3),
  reproduced by the reader several times, NOT YET by a headless run.** Playing
  Qalisar at 4K with the preset: 35-40 fps at the start, then ~16 fps
  (47-60 ms a frame) after walking back and forth between the start
  (15300, -10050) and the end of the pavement toward Anekbah's airlock (~13424,
  -9923), and staying low - in one log, short spikes at frames ~4980 and ~7920
  that recovered, then 47-59 ms held from ~9700 to the close (12350), the
  camera at 13541 -68 -9894 looking west. Established: a live `sample` of the
  slow game - the main thread waits on the GPU's fence (76%) inside
  `drawWithWater` -> `drawWaterScene`, so the GPU frame grew; the floors' plane
  stayed y 0, no area streamed in, the clip memo stays small (161 -> 245).
  On a QUIET machine (the reader's game closed - measurements taken beside it
  were void, a mistake made twice): the far end 58 fps off / 48 on / 52
  unlit or with an empty reflection; standing there 12000 frames, 21 ms
  steady; a scripted walk west and back, 17-30 ms. The lit reflection alone
  adds short SPIKES (to ~34 ms) as the street's motorbikes pass close - none
  with it unlit or the floors off; the reflection's binning is normal through
  a walk (listed a little below the main pass's, no full cluster).
  **The GPU timestamps are built** (2026-10-21, `OMK_GPU_TIMES=1`, Vulkan):
  one at each pass boundary of the frame's command buffer, outside every
  render pass, read after the frame's fence - "vulkan: GPU ms a frame (mean of
  60) - shadow, reflection, scene, post" and `@evt gpu.time`. At the far end,
  quiet, 4K: shadow 0.19, reflection 1.40, scene 6.62, post 3.67 - 12 ms, so
  that frame is not GPU-bound when it is fast. To reproduce, play with
  `OMK_GPU_TIMES=1` and read which pass grows when it slows; then, if it is
  the reflection's lights, a cap on the lights a reflected fragment takes.
  Machine notes: SDL3 appeared on this machine the same day and the Makefile
  prefers it (`make SDL_PKG=sdl2`, or `MAKEFLAGS=SDL_PKG=sdl2` for verify).
* **The game's own rain** (ASSETS 4e, read for the reader): Anekbah, the
  Impasse, the rooftops and Khonsu, one shower every ~11-18 minutes of wall
  clock (8-14 dry, 20 s ramp, 2.5-3.5 min, 10 s fade), only at *Niveau de
  detail* 2 - Qalisar has none. An idea: a rain city's floors wet with the
  cycle.

## Step 3, A5 - the bodies lit by their surroundings: as built (2026-10-20)

The reader's "continue with the character lighting". `[Remaster] bodylight = S`
/ `--body-light S`, and `--remaster`'s **1** (the reader's pick, "keep the
middle ground at 1", from the comparisons below); off unless named, never
`all = max`, Vulkan and GLES (`o3de/lightprobe.h`).

* **The rule.** The original starts every lit body - the crowd, the
  characters, the player, through `sub_494E80` - from ONE grey, the scene's
  `+416` (the set's ambient), and adds the set's lights that reach it
  (`o3de/vertexlight.h`); the set is shaded by colours BAKED into its
  vertices. `LightProbes` reads those: the set's OPAQUE surface (water, glass
  and multiply shadows are not light) summed into 200-unit cells at load BY
  AREA - each triangle cut into pieces no longer than a cell, its corners'
  colour interpolated - and around a body the cells' mean weighted
  (1 - d^2/R^2)^2 on each centroid, R 600 - continuous as he walks. The body
  starts, where the probe is brighter than `mean` (its mean colour at body
  height (90) over the set's floors, by area), from the grey PLUS
  `S (probe - mean)` per channel; where darker, from the grey SHADED
  `1 - S (1 - probe / mean)`; clamped 0..1 - so the average place keeps the
  original's grey and only the departures carry over: a dark stretch darker,
  a lit one brighter, its hue with it. The lights still add on top. Nothing is added to the set.
* **How it got there - three readings, compared by the reader:**
  * a RATIO of the grey, `1 + S (probe / mean - 1)`: "difficult to see a
    difference" - a set's grey can be nearly black (Anekbah's is **10 of
    255**, 0.039) and the lamps outweigh any multiple of it; a contrast curve
    on the ratio (`(L / mean)^(3 S)`) was no better for the same reason;
  * the PROBE ITSELF as the start (`grey + S (probe - grey)`): the most
    visible, and it brightens every body everywhere - the sets are painted
    far brighter than the grey the game gives its characters (0.44 against
    0.04 in Anekbah); not taken;
  * the DEPARTURE ADDED (`grey + S (probe - mean)`), "the middle ground":
    taken at 1. Anekbah's lit stretch at 1: Kay'l's red +33%, green +27%,
    blue +13% (the city's mean is warm, so blue moves least).
  * Read by its CORNERS first, Lahoreh's large floor triangles left spots with
    no corner in reach - the surface by area replaced it.
  * **The reader's play test, 2026-10-20 - "Lighting is not correct near the
    fire"** (Anekbah's gas flame `AGAZ`, (1741, -54, -7517)): Kay'l BLACK by
    it. The flame is a sprite and its glow was never painted into the
    alcove - the probe reads 0.25 there against the set's 0.44 - and ADDED,
    that departure is six times Anekbah's grey: the start clamped to 0. A
    darker place now SHADES in proportion (0.52 of the grey there); the
    brighter side stays added. (The original lights no body by that fire
    either - its grey and the set's lamps only; light from the effect
    sprites themselves would be a feature of its own.)
* **Measured** (`texpack_tool probe`, which prints the tint of a 0.2 grey):
  floor means Anekbah 0.4431, Jaunpur 0.6958, Lahoreh 0.7931, Qalisar
  0.4647, Kay'l's flat 0.6051. Anekbah's street tints (0.99, 0.92, 0.76) at
  (1804, -90, -6890), red 2.21 600 units on, 0.52 by the fire; Lahoreh's street start ~1 (an
  average place). Along 1.2 km walks the tint of a 0.2 grey moves at most
  0.04 per 10 units (its start colour 0.008).
* **The plumbing.** `Draw::lightTint` (1, 1, 1 unless the renderer says
  `drawsBodyTint`); Vulkan: one vec4 after the affines in the pose block
  (nothing moves), `scene.vert` multiplies; GLES: a posing-program VARIANT
  (`uLightTint`) made at init only when asked - the Vita's shader cache
  untouched; the CPU paths (the frontend's posed corners, `lightStart`,
  `poseGeometry`) multiply too. The software reference, GL1 and the 3DS
  never get a tint.
* **Found on the way: the GLES pose self-test drew its bodies BLACK** with
  the variant - a uniform starts at 0 - and every body went the CPU way (70
  pixels moved instead of 5931). The tint is defaulted to 1 at init.
* **Off is the original:** frames byte-identical to the build before
  (`0069efa7`, a worktree) on the software reference, Vulkan and GLES, plain
  and with `--remaster`, Anekbah's street and Jaunpur's canal - 10 of 10,
  and under `--enhance-all` on both backends. At 1, Vulkan moves 3011
  pixels of Anekbah's street, GLES 3042.
* **Under `--enhance-all` (the reader plays with it, 2026-10-20: "from now,
  run it with --enhance-all")** PER-PIXEL lighting shades a body in the
  fragment stage from the scene's grey (`Draw::lit` 1) and never reads its
  corner colour, so the tint did nothing there. A tinted body is drawn in
  mode 2 instead - the shaders' "lit ADDED to the corner colour", unused
  until now - with that colour the grey x tint (the CPU's corners; a
  GPU-posed body starts from the grey and `scene.vert` tints it). The
  traffic keeps mode 1. Anekbah's street at 1: 3250 pixels move on Vulkan,
  3208 on GLES; with it off byte-identical to `0069efa7` on both.
* **Near a lamp the tint is small** (measured, a forced 0.25 barely moved
  Kay'l by the Anekbah lamp without per-pixel lighting): the set's lights
  add on top and outweigh the grey. Tinting the LIGHTS too was rendered and
  not taken - the painted colours already contain the lamps, so it counts
  them twice.
* `engine: body light` new, shown to fail four ways (the Vulkan tint
  dropped, the GLES variant empty, the weight made a step, mode 2 left out
  under per-pixel lighting).

**CONFIRMED IN PLAY** (the reader's "commit", 2026-10-20, with `--enhance-all`). **Open**: the props and
the vehicles (posed by the traffic, untinted); a probe per BONE rather than
per body (a tall body under a lamp); the day/night slots (`sub_41E7A0`
rewrites the scene's grey - the probe reads the baked colours, which do not
change).

## Step 3, A5b - the emitters' light: as built (2026-10-20)

The reader's "continue with the emitter lights", after the body light's play
test by Anekbah's fire ("Lighting is not correct near the fire"). `[Remaster]
fxlight = S` / `--fx-light S` (0..4), `--remaster`'s **1**; off unless named,
never `all = max`, Vulkan and GLES (`o3de/emitterlight.h`).

* **Why.** A set's flames, neon halos and lamp glows are the `.sfx`'s
  ADDITIVE PARTICLES: they light nothing. The original lights a body from its
  scene's grey and the set's `.3DO` lights; none sits at a flame, and the
  flame's glow is not painted into the walls either (the burner's alcove
  reads 0.25 against Anekbah's 0.44). So the fire's corner is dark in the
  original too.
* **Which emitters glow** - read from the effect, not its name: additive
  (mode 4/5) and either a STANDING glow (a life of 2 frames or less, one
  sprite re-made on the spot every frame - a lamp's halo) or a stream whose
  SPRITE is saturated (a flame: the explosion sprite's bright texels 0.55;
  the smoke sprite's 0.22). Judged by the product with the colour ramp
  first, the burner (`agaz`, ramp (17,17,17) -> (88,136,154)) read 0.31
  against the steam's 0.22 - too close; the sprite separates them. Anekbah
  117 glows and 12 flames of 153 (24 steam and smoke dark), Qalisar 89 and 8
  of 104, Jaunpur 15 and 0 of 47, Lahoreh 12 and 0 of 22.
* **The light.** A `Light3do` of the set's own kind appended to the slot's
  table when its emitters are bound (`WorldSlot::setLights` keeps the set's
  own count), so per-vertex (`o3de/vertexlight`) and per-pixel lighting take
  it unchanged. A POINT light: a NEGATIVE inner radius (no shipped record has
  one) shines from the emitter to the lit point instead of along the record's
  fixed direction, and wraps 0.3 round the far side (the walls about a flame
  bounce it), and is NOTHING within 30 units of its emitter, whole past 90 -
  a lamp does not light its own housing. Colour: a glow the sprite's bright
  texels x the ramp; a flame its SPRITE's hue (seen orange - the ramp only
  dims it). Reach 4.5 m (glow) and 9 m (flame), full inside a fifth;
  intensity `S x brightness x 0.8` (glow) or `x 1.2 x min(4, sqrt(count x
  life))` (a flame's particles overlap). By eye: the first strengths (0.6 /
  1.5) lit the whole frame; 0.5 / 0.6 at 6 m the reader found "too weak, the
  fire is big"; now 0.8 / 1.2 at 9 m.
* **The set lit too, per pixel.** On a body alone the light did not show: the
  camera sees Kay'l's back, and the walls stayed dark. Under per-pixel
  lighting the SET's opaque draws take `Draw::lit` **3** - the baked colour
  plus the POINT lights alone (the set's own lights are in its bake). The
  frame's point lights are the 4 nearest the CAMERA within 20 m, faded in
  over the last 5 m so none pops; the set's own lights nearest the player
  fill the other 4 of the 8.
* **Backends.** Vulkan: `scene.frag`'s `litColour` takes the point case and
  a points-only pass (mode 3). GLES: a VARIANT of the enhanced fragment made
  at init only when asked (the program the Vita may cache untouched).
* **Measured** by the burner (1780, 0, -7380) at 800x600 with the preset and
  `--enhance-all`: 329514 pixels move, 4 (Vulkan) / 8 (GLES) darker, the two
  405 apart. Off: byte-identical to `26e042ff` on the software reference,
  Vulkan and GLES, plain, `--remaster` and `--remaster --enhance-all`, by the
  burner and in Qalisar - 14 of 14.
* **The reader's play test, 2026-10-20**, three findings:
  1. the fire too weak for its size - its reach 6 -> 9 m, its intensity
     doubled;
  2. **the sliders BLACK** under `--enhance-all` - not this feature (with
     it off the frames are byte-identical to before) but per-pixel
     lighting's: it drew the TRAFFIC in mode 1, from the scene's grey (10 of
     255 in Anekbah), while the engine never lights a vehicle - per vertex it
     draws in its own colours. The traffic is now mode 2 under per-pixel
     lighting: its baked colour plus the lights (an ENHANCEMENT's fix; the
     original per vertex is untouched);
  3. **a neon's arrow washed out**: its glow emitter sits on the sign's disk
     and lit the disk to white - the self-fade above. And "if the neon now
     emits light, make the grade darker for more contrast": `omktexpack.py
     grades --depth G` raises every channel to G after the city's grade
     (black and white kept, the mids darker); the pack's grades are now
     depth **1.2** (the depth-1 ones kept as `grades-depth1/`).
* **The neons: "very small, it is not visible that they emit light (they
  seem phosphorescent at most)"** (the reader, 2026-10-20 - and "letters not
  emitting light in the original is more a GPU limitation than an artistic
  vision"). The glows lit from were the white HALO sprites; the pink letters
  emitted nothing. Now the SET'S GLOWING SURFACES are lights too
  (`surfaceLights`, built at set load into `WorldSlot::fxSurface`): a texel
  is light past 0.6 of the pack's LIGHT MAP (the bloom's; weighted above it -
  the maps mark every bright painted texel too) or by the NEON rule, bright
  (value past 0.65) and saturated (past 0.55) - Qalisar's pink letters carry
  no light map. Each triangle is sampled on a LATTICE in texture space (2..16
  a side): a sign's letters are thin texels inside a keyed-out quad, and its
  corners sampled the key (the first version found no light at the sign).
  Summed by area into 3 m cells; a cell with 300 square units of light is a
  point light at its centroid, its colour the lit texels' pushed 1.6 from
  grey (a tube's core is near white), 7 m reach, `S x min(1, sqrt(area) /
  120)`. Anekbah ~390, Qalisar 304, Jaunpur ~320. Decoding the pack for
  them added ~110 ms to a set's load (M3) - the colour PNGs, 1024x1024 each,
  decoded beside the GPU's own decode; now `replacements(ts, false)` reads
  the light maps alone and the colours are the shipped palette's: Qalisar's
  load 131 -> 53 ms (21 with the emitters' light off), 217 -> 216 lights
  (2026-10-21). And the frame's light slots
  are 16 now (`View::kMaxGpuLights`; the set's own still 8,
  `kOwnGpuLights`): up to 12 point lights, nearest the camera, where the
  renderer has the room (`maxPixelLights` - Vulkan always, GLES in its
  variant).
* **The reader's play test in Qalisar, 2026-10-20**: "light is not very
  visible (maybe it is already too bright around or the light is not strong
  enough) - the inspiration for this area is clearly Blade Runner and its
  night scenes, the neons one of the main sources of light"; and "some
  lights appear and disappear when I move". Three answers:
  * THE NIGHT: `night.txt` beside the city grades (`omktexpack.py grades`
    writes `QALISAR = 0.5`; `CityGrades::nightOf`, a set's own then its
    city's): with the emitters' lights on, the share of the SET's painted
    light and of a lit body's start that stays (`View::setDim` - Vulkan the
    eye's w in the lights UBO, GLES `uSetDim` in the variant, defaulted to 1
    at init); the point lights add on top whole. The sky is not dimmed.
  * POOLS: a point light's falloff is SQUARED (shaders and `reachOf`), so
    the light gathers near its source and the street between stays dark -
    linear over 7 m, the shop panels' warm light washed the whole ground.
    Surfaces' colour pushed 2.5 from grey (1.6 left the pink sign's light
    pale `ffd6ca`), intensity 1.2, cells 5 m (fewer, stronger).
  * NO POP: the nearest 12 by distance swapped as he walked; a light now
    fades by its distance against the FIRST ONE LEFT OUT (`fxCut`, the 13th's
    distance, which moves continuously): one leaving the 12 has faded out.
* **"Better"; then: the light's colour from the neon texture's colours, and
  the lights too few - "the scene darker than it should be, the neons look
  turned off"** (the reader, 2026-10-20). The colour is now the lit texels'
  at full value weighted by their SATURATION (a white core or a grey texel
  adds light, not hue; pushed only 1.3 from grey). The slots: 64
  (`View::kMaxGpuLights`), the set's 8 as ever (`kOwnGpuLights` - the
  per-pixel enhancement's, not the original's: the engine lights per vertex
  from every light that reaches) and up to 56 point lights within 30 m of
  the camera - only with the emitters' light on, on a renderer that says so
  (`maxPixelLights`; GLES in its variant). With so many adding, the surfaces
  and the glows at 0.5 (1.2 and 0.8 washed the street), surfaces reaching 10
  m. The cost at 4K (M3, Qalisar, `--enhance-all`): Vulkan 92 -> 50 fps,
  GLES 97 -> 50 - above the 30 yardstick; a per-tile light list would win
  most of it back.
* **"Still looks off" - the CURATED LIST** (the reader, 2026-10-20: "go with
  the curated list"). The rules cannot tell a neon tube from a bright painted
  panel - both bright, saturated texels; tuned by numbers the street swung
  between washed and dark, the warm panels outshining the pink sign. So
  `lights.txt`, beside the grades (`SurfaceCuration`): lines `SET TEXTURE
  [U0,V0,U1,V1] = GAIN [RRGGBB]`, `*` any set. A texture is an ATLAS
  (Qalisar's `CATITR05`: a yellow emblem panel, lit windows and the pink
  letters), so an entry names a RECTANGLE in the original's texels; a texel
  takes the first rectangle it lies in, else the texture's line, else - the
  set being curated - no light. Which texels glow stays the rules'; the gain
  replaces the neon boost, the colour the texels'. A set the list does not
  name keeps the automatic rules. `texpack_tool lights <pack> <set.3DO>`
  lists the candidates (lit area, neon share, colour, pack key);
  `omktexpack.py grades` writes the first draft (`CITY_LIGHTS`, Qalisar's 20
  lines, from its atlases) only where there is none. At 4.0 the pink letters
  flooded the street; 2.0 kept it a magenta night. "Much better."
* **The neons' bloom** ("add just some bloom to the neon"): the bloom glows
  where the light maps say, and the pink letters have none. A hook on the
  pack (`TexturePack::setLightHook` -> `curateBloom`, on the pool) writes the
  curated rectangles' texels into the light map by BRIGHTNESS (0 at a value
  of 0.3, whole at 0.7 - the neon rule's 0.65 missed the letters' 0.77-0.86)
  times `min(1, gain / 2)`. At half it did not show; spreading the glow 2
  texels did nothing - the letters are a CUTOUT, their neighbours never
  drawn - so "a bit more bloom" is the preset's bloom 1.3 -> **1.6** (2.0
  washed the disk's arrow out again).
* `engine: emitter light` new, shown to fail four ways (a flame by the
  product's saturation; the GLES variant empty; mode 3 never set; the list
  never covering a set). Part 4, the list and the night, in Qalisar and
  Anekbah. On the way: the check compared the backends at TWO FILTERS -
  `_viewer_args` adds `--filter nearest` to a run naming none, the Vulkan
  harness took it and GLES under `--enhance-all` did not (3346 pixels apart);
  the filter is named now. Whether GLES should honour `--filter` under
  `--enhance-all` is OPEN.

**CONFIRMED IN PLAY** (the reader's "it's good now", 2026-10-20, with
`--enhance-all`: the fire, the sliders, Qalisar's night and neons). **Open**:
the LIGHT POPPING - the frame's point lights are the 56 nearest the camera,
so a far neon lights nothing round it and seems turned off ("set it by the
game's distance culling"): per-TILE light lists, every light within the clip
distance, is the next step (done, A5c); the light maps' decode at a set's
load (done: 131 -> 53 ms); the other cities' lists (drafted); without per-pixel lighting the lights reach
only the bodies; flicker (a flame's light is steady).

## Step 3, A5c - the tiled point lights: as built (2026-10-20)

The reader's "go" on the per-tile light lists, after "the neons look turned
off" and the light popping (the frame's point lights were the 56 nearest the
camera). `o3de/lighttiles`; Vulkan only for now - GLES keeps the nearest-slots
way.

* **What.** On a renderer that `tilesLights`, EVERY emitters' point light
  within the game's CLIP DISTANCE (the reader: "set it depending of the game
  distance culling setting"; 200 m at most, the unlimited clip's) goes to
  `View::points`, faded over the distance's last fifth - no slots, no cut;
  `View::lights` keeps the set's own 8. Vulkan writes them to a storage
  buffer (set 1, binding 3) and BINS them each frame: the screen in tiles
  (32 render pixels, growing so that no more than 64 lie across), each tile
  in 16 depth slices (exponential, 50 to 20000 units), a light joining a
  CLUSTER only where its sphere reaches the cluster's own box of view space
  (the tile's rays between the slice's depths); the lists (binding 4,
  16 MB) at most 64 a cluster, the nearest IT. `scene.frag` reads its own
  cluster's list (`vDepth` its slice) - every light within reach, at the cost
  of the ones that can reach it.
* **The steps the reader's play tests took it through:**
  1. 2D tiles alone: 880 of 880 tiles at their cap - a near light covers most
     of the screen and crowded the far ones out again. Depth slices: still
     7920 of 14080 clusters full.
  2. "The lights disappear when I put a resolution higher than 1512x982":
     the lists grow with the resolution and overflowed the 4 MB buffer,
     and the frame dropped every point light silently. Tiles now grow with
     the resolution (64 across at most - 60 px at 4K), the buffer is 16 MB,
     and an overflow is SAID.
  3. "Some kind of flickering big shadows": full clusters dropped lights as
     the camera moved, a block of street at a time. The cluster BOX test and
     the ranking by distance to the cluster: 0 clusters full in Qalisar, the
     lists 8x shorter (50883 entries against 421104 at 1280x960), and the
     frame byte-identical to `OMK_TILES_LOOSE=1` (no test, no cap: 725870
     entries) - a light the box test leaves out reaches none of the cluster's
     fragments. "It looks good now."
  4. Found on the way: the tiled loop PRESENT in `scene.frag`, never run,
     changed a plain frame through MoltenVK - a cutout sign's keyed texels
     drew dark (4317 pixels in Qalisar; bisected to the loop alone). So the
     shader is compiled TWICE (`scene_tiled.frag.inc`, -DOMK_TILED) and the
     tiled pipelines bound only on a frame whose tiles are on; the default
     module is the commit before's.
* **Off is the original:** byte-identical to `18da7d4d` on the software
  reference, Vulkan and GLES, plain, `--remaster` and `--enhance-all` (fx
  off), by Anekbah's burner and in Qalisar - 14 of 14.
* **Cost** (M3, 4K, Qalisar, `--enhance-all` and the preset): 48 fps (the
  nearest-56 way 50, 32-px tiles before the box test 56 but with half the
  clusters full).
* `engine: emitter light` part 5 (more than 200 lights tiled, 0 clusters
  full, tight == loose byte for byte), shown to fail (the box's vertical
  bounds' sign flipped - tight and loose part); its backends' agreement sets
  Vulkan beside GLES with `OMK_NO_TILES=1` (GLES does not tile yet).
  `licence headers` 644 -> 646.

* **The four cities' lists** (the reader's "continue", 2026-10-21): Qalisar
  was the only city curated, the others on the automatic rules, which lit
  white walls. First drafts from each city's atlases (`texpack_tool lights`
  and its textures beside their light maps), in `omktexpack.py`'s
  `CITY_LIGHTS`: ANEKBAH 20 lines (the orange letters and the neon star
  strong, the signs and the lamps bright, the lit windows low, the paintings
  out); JAUNPUR 7 and LAHOREH 5 - sunlit cities, their signs alone at 1, the
  white sandstone and pale walls the light maps mark left out. Kay'l's flat
  (AAPKAYL) is named by none and keeps the automatic rules. Before/after by
  eye: Anekbah's street loses the warm-green wash and keeps pools by its
  lights; under Jaunpur's bridge the walls' false light goes. `engine:
  emitter light` part 4: Anekbah curated, Kay'l's flat not.

**CONFIRMED IN PLAY** ("it looks good now", 2026-10-20, `--enhance-all`).
**Open**: the three new lists by the reader's eye; GLES tiles (float textures for the lists, desktop and Quest); the
CPU binning's per-frame allocations; a light's own shadow (none - a lamp
lights through walls within its reach).

## Step 3, B3 - the relief (normal maps): first cut (2026-10-21)

The reader's "go with the normal maps". No generated maps yet: the first cut
reads the texture's own BRIGHTNESS as height in the shader, so it needs no
asset, no pack format and no tangents, and shows whether relief is worth the
pack's maps (DeepBump and the like, the plan above) at all.

* `[Remaster] relief = S` / `--relief S`, 0..4, **0 by default**; the
  `--remaster` preset's **1** (**2** since the generated maps, below) - the reader's "ok" (2026-10-21) on a sheet of
  0, 1, 2 and 4 by Anekbah's burner and in Qalisar (4 too harsh; Qalisar's
  wide pink light shows little at any strength; the lit floor brightens a
  little as it grows - a bias to correct if a higher strength is wanted). `View::relief` ->
  the Lights UBO's `fx.x` (Vulkan).
* `scene.frag` `relief()`: four taps a texel apart on the BOUND texture (the
  pack's 4x where there is one), the slope x8 a texel, tilting the normal in
  the COTANGENT frame of the surface's screen derivatives (world position,
  UV) - the sets carry no tangents.
* Only the POINT lights see it (the painted light, the bake, has no
  direction), so it needs the emitters' light and per-pixel lighting
  (`--enhance-all`); only on the SET (mode 3) - Kay'l's shoulder pad and arms
  took grain from their painted texture; only in Vulkan's TILED module
  (`#ifdef OMK_TILED`, the MoltenVK lesson of A5c). GLES has none.
* x4 was barely seen: the median moved pixel 8 of 255 at 1, a crop at full
  size showed the wall's grain only at 2-4. x8 makes 1 what 2 was.
* Off: byte-identical to `e0b586b1` on the software reference and Vulkan,
  plain, `--remaster` and `--enhance-all`, by Anekbah's burner and in Qalisar
  (12 of 12). `verify.py: engine: relief` - the source, relief 1 moving the
  frame by the burner (81875 pixels at 640x480), and nothing without the
  point lights or the tiles; shown to fail three ways.
* Open: the strength and the sign (bright reads as raised - true of stone
  and mortar, not of a lit window); whether generated maps are worth it
  after this; GLES.

## Step 3, B3 - the generated normal maps: as built (2026-10-21)

The relief's second step: a NORMAL MAP per pack texture, `<key>_n.png`
beside its colour, read by the same `relief()` where there is one (the
brightness elsewhere). **The preset's relief 1 -> 2** - the reader's
"relief 2 is better" once the maps came.

* **The generator** (`engine/src/o3de/normalmap.{h,cpp}`, `texpack_tool
  normals`): the reader's pick from a sheet of seven textures - DeepBump's
  map (HugoTini, GPL-3, colour to normals; it reads VOLUMES but invents
  blotches and streaks) blurred until its inventions are gone, as the large
  forms, plus the engine's own band-passed brightness slope over four scales
  as the grain. Without DeepBump the map is the engine's alone, which anyone
  can rebuild. OpenGL's convention (R +u, G the image's up), the height in
  alpha. `tools/omktexpack.py materials PACK --deepbump DIR --python PY`;
  DeepBump's maps cached in `PACK/../materials-ai/` (1259, the long part),
  the whole pack regenerated from that cache in 79 s (M3, 8 jobs).
* **The engine**: `TexturePack` lists `_n` files beside `_e`; decodes them
  only when the session asked (`setNormals`, relief above 0) - another
  1024x1024 PNG per texture. Vulkan: binding 3 of a texture's set, a flat
  1x1 of ALPHA 0 where there is none (the shader's "none"); the descriptor
  pool 390 -> 530. GLES has no relief.
* **The floor's line** (the reader's "do it first"): a crisp groove along
  some tile edges of Qalisar's pink street. Found by bisecting the pack's
  maps (11 renders): `BATITR09`, an ATLAS - beige paving, a long green
  slab and an unused bolted plate in one 256x256. DeepBump draws a wide
  ridge where two pictures meet; a floor face maps the beige paving up to
  its border, and at a grazing angle the sampler's coarse mip reaches into
  the ridge. NOT the cause, measured on the way: the upscaler's border (the
  pack's edge texels step 2-14x an interior step, but the ORIGINAL's own
  wrap seam, sharpened) and DeepBump's unwrapped edges (handing it a
  wrapped margin of an eighth changed nothing - removed again).
* **The fix - the faces' REGIONS** (`texpack_tool islands <data> <dir>`,
  `<key>.cov`, made once into `PACK/../materials-cov/`, 33 s): every face's
  UV triangle over every model, rasterised by texel centres; triangles
  sharing a centre a texel or more inside each are ONE region (union-find),
  so the beige and green, which meet on a line, are two. 1253 textures,
  664258 faces, 48484 regions, 3.5% of texels a border. The generator
  fades the slope flat over 16 texels (at 1024 across, four of the
  original's) from a border or an unsampled texel: 8 left the line, 16 and
  32 took it. 0 of the 664258 faces spans more than its texture - the sets
  do not tile a texture across one face.
  Tried and dropped: a UV edge only one triangle has (170786 - the sets cut
  their faces freely, most such edges inside a picture); a 3D edge across
  which the texels jump (567299 - the paving restarts at nearly every face:
  Qalisar's floor, 5003 shared edges, 895 jump, 2123 one-sided).
* Off: byte-identical to `b39143ac` on the software reference and Vulkan,
  plain, `--remaster --relief 0` and `--enhance-all --relief 0`, by
  Anekbah's burner and in Qalisar (8 of 8). `verify.py: engine: relief`
  part 4 - the binding in the tiled module, normal maps uploaded at relief 1
  (39 by the burner) and none at 0; shown to fail (`setNormals` never
  called: 0). Relief 1 still moves 85210 pixels there.
* **The freeze entering a bar in Qalisar**, met in the play test (`b39143ac`):
  the bar's set loads on a `BackgroundJob` whose pack decode is a
  `parallelFor`, beside the frame's own on the main thread; the pool held
  one call's state and the loader waited for ever. Older than the maps -
  since the decode went onto the pool (`30f2328e`) - which made the window
  long enough to meet. A call lock per pool; `verify.py: thread pool`.
* Open: a faint trace of the line remains at relief 2 at a grazing angle;
  the one stretch of BATITR09's beige/green border no region parts (a face
  maps across both, by the looks); the sign (bright as raised) in the
  engine's grain; GLES.

## The review of steps 1-3 (2026-10-21)

The reader's "recheck everything you did for the remaster". Four read-only
reviews (the Vulkan backend, GLES and the plumbing, the CPU side, the tools
and checks), each finding re-read in the code before a fix; one rejected.

**Fixed:**
* `TexturePack`: `refused_` and `greyOf_` raced (a set's load on a
  BackgroundJob against the renderer's call and `aliasGrey`) - a mutex; a
  refused COLOUR no longer drops the key's light map; light maps decoded
  with the colours only when a bloom reads them (`setLights`), no normal
  maps for GLES (no relief there).
* The pool: a call finding it held by another thread runs INLINE rather than
  wait (the frame stalled for a set's decode); the Vita's caller count, now
  raced by two callers, replaced by the owner's thread id and a polled
  semaphore.
* The bloom: what no surface covers cleared to alpha 0, not 1 - the fog past
  the clip distance glowed as light (Vulkan and GLES; 0-226 pixels moved in
  three views, none by more than 8).
* GLES: the variants built over BOTH program sets, so the plain fallback
  links when the enhanced one does not (it inherited the enhanced fragment
  and GLES did not start); the light-map binding cache reset when textures
  are dropped; light maps freed at shutdown; the water's two uniforms
  through the cache; the post-pass's uniforms looked up once.
* Vulkan: a failed grade upload rebinds the post sets (they held a destroyed
  view). `Renderer::drawsWater()` asked before the shadow pass.
* `curateBloom` / `surfaceLights`: the curation's lists found once a texture,
  not by name per texel (a string each). `.cube`: `LUT_3D_INPUT_RANGE 0 1`
  read (Resolve's); the night factors cleared on a re-open.
* `omktexpack.py materials`: maps and DeepBump's written under another name
  and renamed, `islands` finished by a `.done` - an interrupted run left
  files a later one trusted.
* The checks: six remaster checks took "no frame" / "no vulkan device" as
  their own answer - a crashed viewer passed (`_or_skip`: only a backend not
  tried at all may stand in); water and body light compared GLES at bilinear
  against Vulkan at nearest and shared one saves folder; the pack through
  `omkpaths.pack_path()` (`$OMK_PACK`, `pack =`); timeouts. New `engine:
  normal maps` (the generator and `islands`, shown to fail both ways).
  `engine: bloom and grade` had been red since the AO (`|| aoOk_`).

**Rejected:** "GLES claims the emitters' light it cannot draw" - mode 3 needs
per-pixel lighting, which a failed enhanced link already refuses
(`playsetup_bodies.cpp`); and the emitters' light reaches the BODIES on GLES
per-vertex through the set's light table, which a refusal would have lost.

**The second pass** (the reader's "then do the second pass"; everything on
byte-identical to `4eb586ae`, 18 of 18 - the burner, Qalisar, Jaunpur's
canal; software, Vulkan, GLES):
* The light binning (`lighttiles.cpp`): one flat list of (cluster, distance,
  light) and a STABLE counting sort, its scratch kept from frame to frame -
  one vector a cluster (~37k at 4K) was allocated every frame; the words are
  the same bit for bit. The reflection's `begin` bins nothing (the frame's
  pass overwrote it and the reflection's shader skips the tiles).
* Under the AO the water reflection has its own render pass, its depth not
  stored (`reflPass_`; store ops leave the pipelines compatible).
* `setTextures`: light maps, normal maps and the 1x1 stand-ins uploaded
  without a descriptor set (each allocated, written and freed at once), and
  a failed allocation said and drawn white instead of written into.
* `makePost` remembers a failure for its size (it rebuilt and said so every
  frame); `LightUbo` sized by `View::kMaxGpuLights` and its offsets pinned by
  a `static_assert` (the comments said 400 / 416; they are 3088 / 3104).
* GLES: a variant whose anchor is not in the shader SAYS so (it built and
  did nothing).
* Shared: `lutStrip` (`colourgrade.h`, the grade's strip, written twice);
  `[Remaster]`'s keys through two helpers (`settings.cpp`); the light
  selection's and `drawWithWater`'s vectors kept between frames.
* `omktexpack.py`: `anchor()`'s docstring said one pass restores the box
  averages - it halves their error (4.96 -> 2.56; three passes 1.05);
  `build --anchor-passes N` for a rebuild, 1 the default (the judged pack);
  the emissive rule's medians over the drawn texels; argument errors said.
  `texpack_tool`: `keys` and `islands` share the mesh walk, `pngpar` refuses
  a count below 1, the usage names every mode.

Not done, and why: the water reflection's image in its own pass's sets - the
shader never reads it there (`pc.water == 1` is false), and the spec's
layout and feedback rules bind only descriptors a draw ACCESSES; one helper
for the session's flag / key / preset picks - three checks read those lines;
a memo of `source_textures` - the build walks keys, not models.

**The anchor-3 rebuild (2026-10-21), NOT ADOPTED, the reader to judge.**
`build --anchor-passes 3` into `omk-pack/full-anchor3/` (1255 textures, the
`_e`/`_n` maps linked from `full/pack`), against the judged pack (1 pass):
every texture differs, mean |diff| median 1.04/255, p90 2.07, max 7.78. The
sheet `full-anchor3/compare-sheet.png` (original | 1 pass | 3 passes; the
six most changed and two typical) shows what the number says: three passes
pull the noisy textures back toward the source's pixel grid - blockier on
the moss/water/stone grains, no visible change on drawn ones. To try it in
play: `--texture-pack ~/Documents/omk/omk-pack/full-anchor3/pack`.

Still open, the reader's to choose: the reflections (water and the authored
mirror) without the tiled point lights - a binning for the reflected camera
(DONE for the water and the floors, A3 continued); water and an authored
mirror in one frame; the light maps uploaded as one channel (R8 / luminance),
a quarter of their memory - **DONE 2026-10-21**: Vulkan `R8_UNORM` where the
device blits and filters it (`r8Ok_`, else RGBA as before; `@evt texpack.pool
lightChannels=1` on the M3), GLES `GL_LUMINANCE` (as `maskTex_` already was);
the shader reads `.r` - byte-identical to `1f3ff2fa` with the bloom on, 8 of
8 (Vulkan and GLES, plain preset and `--enhance-all`, the burner and Qalisar);
a 1024x1024 map 4 MB -> 1 MB.

## Step 3, A4 - the ambient occlusion: as built (2026-10-21)

The reader's "continue" after B3; confirmed in play at 2 ("commit, push").

* `[Remaster] ao = S` / `--ao S`, 0..4, **0 by default**; the `--remaster`
  preset's **2** since 2026-10-21 (the reader's "add ao 2 to the preset" -
  first left out, the reader having played it at 2 and named no strength).
  Vulkan only; GLES has none.
* The frame's DEPTH, kept only when asked: `makeTarget` makes it SAMPLED and
  STORED under `aoOk_` (the format's sampling checked), with a depth-aspect
  view. In the post pass (`recordPost`, run when bloom, grade or AO is on):
  `ao.frag` mode 3 reduces it to a HALF-size view z (R32F; sample 0 of the
  multisampled depth - `-DOMK_MS`, a second module), mode 0 the occlusion
  (8 samples on a disk of 60 game units, at most 4% of the picture's width,
  the Alchemy estimator, faded past 80 m and by the game's own FOG), modes
  1 and 2 two blurs weighted by depth; `post.frag`'s final step MULTIPLIES
  the frame by it, not on a light map's texels (the bloom's alpha).
* Contact only: the sets' vertex colours hold the painted occlusion. At 4 a
  dark halo spreads round the feet; a character standing ~30 cm from a wall
  darkens from 1 up (physically right - "characters cast but never receive"
  is offered, not built).
* Cost (M3 loaded, 4K Qalisar, preset + `--enhance-all`): 44.7 fps off, 40.3
  at 2 (2.4 ms). The first cut read the 4K multisampled depth 17 times a
  pixel: 31 fps; keeping the depth alone costs nothing (45).
* Off: byte-identical to `7ac3b75f` (12 of 12). `verify.py: engine: ao`,
  shown to fail two ways.

## B2 - the curation: the mechanism and a draft, NOT applied (2026-10-21)

The reader's choice: "build the tool + draft, don't apply" - which textures
leave the pack is theirs, by eye.

* **The mechanism**: `PACK/exclude.txt`, a 16-hex key a line (`#` comments):
  a listed texture draws AS SHIPPED - its colour and its normal map left out,
  its light map kept (read off the original's texels); the engine says how
  many ("left as shipped by exclude.txt", `@evt texpack.open excluded`).
  `texpack_tool open <pack>` answers the counts; `verify.py: texture pack
  curation` (a made pack of two, shown to fail two ways - the file unread,
  the normal map kept).
* **The draft**: `omktexpack.py curate ROOT` writes `ROOT/exclude-draft.txt`
  (48 keys, outside the pack: nothing applied) and contact sheets
  `ROOT/curation/sheet-NN.png`, the original beside the pack's. The score: the
  ORIGINAL's "ink" (luma 40 from its 5x5 mean) cut into glyph-sized marks,
  counted only in LINES (four or more on a baseline, alike in height, close) -
  the first cut counted every small mark and drafted noisy weapons and
  mottled walls - plus a bonus for names that say lettering (TEXT, MAPS, PLAN,
  ECRAN, LOGO, AFFICH...). Its image half finds real labels (PASSP1, PHARMA1,
  BATTERIE, TETRA02, OMNIAM) and some false ones (FOIE, MEGAZ1).
* **For the reader**: read the sheets in `~/Documents/omk/omk-pack/full/
  curation/`, move the agreed lines of `exclude-draft.txt` into
  `full/pack/exclude.txt`.

## Step 3, A4 continued - the mist: as built (2026-10-21)

The plan's "height or volumetric fog in the game's own fog colour and range";
the reader's "0.5 looks fine" on a sheet of 0, 0.5 and 1 by Anekbah's burner,
on Qalisar's street and at Jaunpur's canal.

* `[Remaster] mist = S` / `--mist S` (0..1), the `--remaster` preset's
  **0.5**, off unless named; Vulkan only (both scene modules - `fx` declared
  in the plain one too, which changed no frame: 15 of 15 off byte-identical
  to `5f4cb214`). Named apart from the game's fog.
* **The colour is the game's own fog colour**, per city and per the clock
  (`sub_41E7A0`) - at the save's clock, phase 2: Anekbah 12 25 35, Qalisar 19
  10 38, Jaunpur 12 35 48. Jaunpur's dark teal looked wrong beside its golden
  sky texture, and is the city's own night fog (the reader: "the fog colour
  is not the same on each city"); it follows the clock with nothing curated.
* **The shape**: density 1 at the set's GROUND (`groundLevel` - the height
  with the most level opaque area: 0 for Qalisar, Anekbah and Lahoreh, -120
  for Jaunpur's quay) falling as exp(-h / 400) above it (Y down), the ray's
  mean density in closed form; 15 m to 63% at strength 1 (40 m read as
  nothing over a street).
* **Which batches**: the game's fogged ones - and with its fog OFF (an
  unlimited clip, which `--enhance-all` sets) the same ones (not the near
  bucket, not the transparent state - key 0x2080) marked -1 / -1 by the
  backend: the first cut took only the fogged ones and drew nothing under
  `--enhance-all`. Not in the water's reflection pass.
* `verify.py: engine: mist` - the source; in Qalisar (its fog off, the
  post-pass off) `--mist 0.5` moves 260094 pixels toward the fog colour (48
  away: a street lamp's ADDITIVE halo, which the mist leaves as the game's
  fog does, over a background it moved) and its shape - the near ground 5.7,
  the horizon band 21.5 a pixel; shown to fail two ways (an inverted mix
  passed the first form). `engine: body light` and `engine: emitter light`
  compare Vulkan with GLES at `--mist 0`.
* Open: GLES; the mist in the reflections; a lighter haze by day if the
  day's fog colours want it (unseen: the save's clock is evening).

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
