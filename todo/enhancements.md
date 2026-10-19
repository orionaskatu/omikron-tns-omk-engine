# Enhancements - what the original never had, all OFF by default

The reader's rule (2026-09-08): anything the 1999 game did not draw stays
optional, behind a launch flag on `omk-play` or a key in the config file's
`[Enhancements]` section - a category of its own, beside the engine's
`[Preferences]` and this port's `[Options]` stand-ins for real menu rows.
The GPU backends draw them - Vulkan, and since 2026-10-05 GLES too (the
section below, with what the Vita's vitaGL cannot do); the software reference
stands where D3D stood and draws what `sub_4638C0` told D3D to
(`docs/ASSETS.md` 4).
Each one gets a `verify.py` check that pins the default OFF in the source and
measures the enhancement's own property on the GPU, shown to fail.

| # | enhancement | key / flag | status |
|---|---|---|---|
| — | **all of them at once** but supersampling (row 9, asked for by name only since 2026-10-01), each as high as it goes. A BASE: any specific key or flag beats it, whatever the order, and it touches nothing outside `[Enhancements]` | `all = max` / `--enhance-all` | **done 2026-09-09**; `enhance all` |
| 0 | anti-aliasing (MSAA 2/4/8) | `antialiasing = N` / `--aa N` | **done 2026-09-08**, `ed349ca`; `engine: anti-aliasing` |
| 1 | ~~bilinear texture filtering~~ - **NOT an enhancement** (2026-10-03): it is what the original's hardware device drew (ASSETS 4), and since that day the DEFAULT on every GPU backend, GLES and GL1 included. The colour key (flag 0x800, black) travels in the texture's ALPHA so a filtered sample is premultiplied: discard below 0.5, divide by alpha above - no dark fringe (GL1, fixed function, cannot divide: up to half the key's black at a cutout edge), and the nearest path is bit-identical to before | default; `texturefiltering = nearest` / `--filter nearest` for the software devices' picture | **done 2026-09-08, default 2026-10-03**; `engine: texture filter`, `engine: gl1 backend`; judged by eye on Aapden's floor stain |
| 2 | mipmaps (trilinear) and anisotropic filtering, generated at upload; the alpha key averages correctly into the chain | `texturefiltering = trilinear`, `anisotropy = N` / `--filter trilinear --anisotropy N` | **done 2026-09-08**; `engine: mipmaps`; the stain judged by eye at 16x |
| 3 | interface scaling: the 640x480 layer's stretch FILTERED instead of nearest. The key decides the silhouette and the blend decides the colour, so a keyed edge cannot fringe without an alpha channel to premultiply into. The one row that is NOT Vulkan-only. An INTEGER/centred mode is not done and is its own step - see below | `uiscaling = linear` / `--ui-scaling linear` | **done 2026-09-09**; `engine: ui scaling` |
| 4 | unlimited draw distance: options row 3 is a CAP the port already runs the visible-set walk from; 0 lifts it, and the FOG with it, the fog's range being the clip's. **Measured, it buys nothing at 200 m**: the shipped sets have no sightline past the option's own maximum, in any of the four cities - see below | `clipdistance = 0` under `[Enhancements]` / `--clip 0` | **done 2026-09-09**; `engine: unlimited clip` |
| 5 | **fitted shadows**: the same blobs, laid on the surface actually under them instead of on a flat quad at the probed height | `shadowquality = fitted` / `--shadow-quality fitted` | **done 2026-09-09**; `engine: fitted shadows` |
| 6 | **mapped shadows**: a real shadow map, cast by the set's own authored lights, characters only | `shadowquality = mapped` / `--shadow-quality mapped` | **done 2026-09-09**; `engine: mapped shadows` |
| 7 | **per-pixel lighting**: the engine's OWN light law evaluated per fragment instead of per vertex (the engine lights every character and prop too, not the crowd alone - `todo/drift-audit.md` L1), starting from the scene's ambient grey (`View::litBase`) | `lighting = perpixel` / `--lighting perpixel` | **done 2026-09-09**, the start colour and the characters' mode corrected 2026-10-06; `engine: per-pixel lighting` |
| 8 | **the SETS receive the lights too.** Held back deliberately - it overrides authored art; see below | `lighting = sets` | not recommended |
| 9 | **supersampling**: render N times larger each way and average down. Reaches the CUTOUT edges MSAA never looks at, and the texture aliasing it cannot touch either | `supersampling = N` / `--ssaa N` | **done 2026-09-09**; `engine: supersampling` |
| 10 | **the shoot radar in every arena**: shoot mode's minimap wherever the area ships a radar file. The game shows it only when a script's op 146 turns it on - bare in the Archives, and in the seven other arenas only for object 980, which nothing in the game gives (`ui/radar.h`, `todo/shoot-mode.md` 8.3) | `radar = always` / `--radar always` | **done 2026-09-10**; `engine: shoot radar` |
| 11 | **60 fps**: the viewer's 30 Hz pacer lifted. The ORIGINAL has no cap at all - a vsynced `Flip`, a delta of `30 / fps` - so this is what the 1999 game did on a fast machine; it is listed here because the port's default stays 30. The frame-locked sites it exposes are fixed first, at every rate (`todo/sixty-fps.md`) | `framerate = N` / `--framerate N` (`--fps` already SHOWS the rate) | **done 2026-10-01**; `engine: frame rate` |
| 12 | **bodies smoothed between keys**: a pose slerped from key `k` to `k+1` by the frame's fraction. The engine TRUNCATES (`Anim_ApplyNodeFrame`'s `_ftol`), so at 60 its bodies glide on fractional root motion while the pose steps at 30 - this is the part the original never had | `animation = smooth` / `--smooth-anim` | **done 2026-10-01** for the player, the scene bodies and the crowd (`todo/sixty-fps.md` 4); `engine: frame rate` |
| 13 | **text scaling**: the glyphs scaled with the layout. The original scales the interface's COORDINATES (`I2D_ScaleX/Y`) and draws every glyph at its native size, so text shrinks as the display grows - at 4K a sixth of its 640x480 size. `fit` scales every metric `TextLayout` hands out (advance, kerning, line height) and every glyph it draws by the smaller of width/640 and height/480, so the wrap, the alignment and every caller move together; filtered (the COVERAGE interpolated before the ramp) when `uiscaling = linear`, and that coverage then SHARPENED - pushed through a curve `scale` times steeper about one half and smoothstepped, so the interpolated contour keeps its shape and the edge narrows from four or five screen pixels at 4.5x to about one (a reader, 2026-10-02: "a little blurry"; what distance-field fonts do, the coverage standing in for the field). A scaled glyph's EDGE is blended over what is behind it (`(colour*c + behind*(31-c))/31`) rather than darkened by the engine's ramp, whose one-pixel dark rim becomes a visible outline at 4.5x (a reader, 2026-10-02); over pure black that is the ramp's value to the bit, and the native path is untouched. Both backends. In `all = max` | `textscaling = fit` / `--text-scaling fit` | **done 2026-10-02**; `engine: text scaling` (the INKED width, exactly 2x at 1280x960 - the composer's advance reads the layout's intention and could not fail) |
| 14 | **ESC / BACKSPACE as BACK**: the interface's BACK (bit 0x20) is the `Annuler / Sauter` binding alone - SPACE, joystick button 49 - and ESC is polled by `Game_RunLoop` for the pause screen and nothing else, so from the sneak's verb panel (its builder 0x0049B810 disables the tab column) ESC leads nowhere (`todo/sneak.md` 2g-bis). `on` sends BACK on a fresh ESC or BACKSPACE while a screen is up; the edge is taken every frame, so the ESC that opens the pause does not close it, an ESC taken as BACK latches until released so it cannot reopen the pause, and BACKSPACE is left to a focused name field. In `all = max` | `escapeback = on` / `--escape-back` | **done 2026-10-17**; `engine: escape back` |
| 15 | **SSAO** (screen-space ambient occlusion): the soft contact darkening of creases, corners and where a body meets the ground. The original has none - the sets' baked vertex colours carry the artists' own shading, radius 80 units (inches, ~2 m) so the architecture's creases and corners read, near samples weighted most and occluders counted only within the radius IN 3D so a character darkens only round its feet, faded out past 3500 units where the street is fogged; `OMK_SSAO_VIEW=1` shows the occlusion itself. Vulkan only: `end()` keeps the frame's depth and runs `ssao.frag` (16-point hemisphere from depth-rebuilt positions and normals) then `ssao_blur.frag` (4x4 depth-aware average, multiplied into the frame by the blend). Under MSAA the multisampled depth is read at its first sample (`ssao_ms`, glslc -DOMK_MS). In `all = max` | `ssao = on` / `--ssao` | **done 2026-10-18**; `engine: ssao` |
| 16 | **Best LOD for the crowd**: a pedestrian model holds four skeletons of falling detail and `sub_48D7F0` steps one down for every 10 m of view depth (`kLodDistances` 10/20/30/40 m), so past 10 m every walker is a coarser body. `best` clamps the level to 0 at any distance - the path `OMK_PED_LOD_MAX=0` already showed draws exactly what no LOD draws. Vehicles already draw their heaviest sub-object (`lodBase`), and named characters have no chain. In `all = max` | `lod = best` / `--best-lod` | **done 2026-10-18**; `engine: best lod` |
| 18 | **Temporal upscaling - AMD FSR / NVIDIA DLSS**: the frame drawn at a RENDER size below the display size (the SDK's answer for the quality: native 1.0x, quality 1.5x, balanced 1.7x, performance 2.0x, ultra performance 3.0x) over a per-frame sub-pixel jitter, and rebuilt at the display size from its colour, depth and motion vectors. The Direct3D 12 backend only (`--d3d12` / `renderer = d3d12`; `--world-d3d12` headless) - Vulkan declines it and says so. The scene pass writes the motion vectors as a second target (`scene.vert`/`scene.frag` built -DOMK_MOTION: the previous view-projection, and a posed body's previous affines after its current ones in the pose buffer; the set, particles and CPU-posed bodies carry camera motion only); SSAO runs before the upscale, the HUD after it on the CPU. FSR through the FidelityFX loader, which picks FSR 4 on RDNA 4 (4.1.1 on the RX 9070 XT), DLSS through Streamline (manual hooking, signature-checked, written to the SDK and not run: an AMD GPU answers 'unsupported' and falls back to FSR); `kFeatureDLSS_NR` is queried and logged. Supersampling and MSAA give way to it. History reset on a camera jump (5 m or 30 degrees in a frame), a set released and a resize. `OMK_MOTION_STATS=1` sums the vectors up. NOT in `all = max` | `upscaler = fsr|dlss`, `upscalequality = ...` / `--upscaler`, `--upscale-quality` | **done 2026-10-18**; `engine: upscaler` |
| 19 | **Hybrid ray tracing** (Direct3D 12, DXR 1.1 inline ray queries): the raster frame as before, with rays cast from its passes against acceleration structures made from what the frame submits - a BLAS per set geometry (once, from all its opaque and cutout batches, so what is culled from view still counts; remade on a revision change), ONE BLAS for every posed body within 115 m (posed on the CPU by the affines `scene.vert` applies, refitted while the same bodies make it, rebuilt on a change and every 30 frames), one TLAS a frame (sets mask 1, bodies 2; a cutout geometry is non-opaque and the ray reads its colour key). Three effects (`backends/d3d12/shaders/rt.glsl`, the -DOMK_RT builds of `scene.frag` and `ssao.frag`): (1) a CHARACTER LIT BY ITS SURROUNDINGS - 6 cosine-weighted rays a fragment into the set, a hit returning the baked vertex colour x its texel (bindless: the records and textures through two unbounded tables over the heap), a miss the fog/clear colour, the mean added at a Lambertian's strength 1, the 2x2 quad averaged through fine derivatives (no history, no shimmer: the rays turn per pixel, never per frame), and the rays stopped OVERHEAD within 10 m taking up to 30% of his ambient (under an arch); (2) SHADOW RAYS for the lamps: one ray (jittered over an 8-unit lamp, stopped 24 short of it) per contributing lamp against the set; a per-pixel-lit body skips a hidden lamp in `litColour`, a vertex-lit one (the default) is scaled by the ratio of its lamps traced to its lamps open (the frame's lamps are handed to the renderer whatever the lighting). The set's two SKY FILLS (reach 35787, 20-30 km up) are not lamps and are not traced - shadowing them put the player in the shade of a block 100 m off; (3) RAY-TRACED AO in SSAO's place - the same pass, radius 80, 4x4 blur and multiply, 4 rays a pixel from the depth-rebuilt position, the bodies occluding too. The set keeps its baked light; only AO touches it; the mapped shadow still darkens the ground alone. 1280x720 on an RX 9070 XT, 600 frames `--nodelay`: 9.2 s without, 11.2 s with (+3.4 ms a frame: ~0.9 ms the bodies' BLAS, ~1.2 ms the AO rays, ~1.1 ms the character rays). `OMK_RT_NO_INDIRECT=1`, `OMK_RT_NO_SHADOW_RAYS=1` and `OMK_RT_TUNE=` for judging. In `all = max` (Vulkan and the software reference say it is D3D12's and stay off; a device without DXR 1.1 says so) | `raytracing = on` / `--raytracing` | **done 2026-10-18**; `engine: raytracing` |
| 20 | **Vehicle shine**: the sliders and the motos get a Blinn-Phong highlight from each lamp in reach and a sky reflection (a gradient from the scene's ambient and fog colour, darker below the horizon) under a Fresnel weight, added after the body's own lighting in `scene.frag` (`Draw::lit` bit `kDrawShine`, set for the road traffic's `VehStaged` bodies whose model is `sli_fn` or `moto`). The game has neither: a vehicle's meshes carry flags 0 and white vertex colours, and the shine you remember is painted into `SLI_FN.3DT`; the `0x4000000` environment-map pass that would reflect is dead in the shipped build (ASSETS 4c). Both GPU backends (the one GLSL); the camera rides in the lights uniform. In `all = max` | `vehicleshine = off|on` / `--vehicle-shine` | **done 2026-10-18**; `engine: vehicle shine` |
| 21 | **True 32-bit colour output**: the 3D was always drawn into RGBA8 targets from 24-bit textures and palettes; what made the game look 16-bit was the PRESENT - `present.frag` (Vulkan and, cross-compiled, D3D12) quantised every pixel to RGB565 with the 4x4 Bayer dither and re-expanded it by bit replication, the original's 16-bit device reproduced, and a frame with anything over the 3D was composed on the CPU over a 565 readback (`ui/surface.h`). At 32 the present pass hands on the supersample/upscaler mean as it is (a `depth` push constant; 16 is the old arithmetic, untouched), and on Direct3D 12 the soft-gated frames (fades, subtitles, media lines, conversations, HUDs, open screens) go out as the GLES window's OVERLAY: the 2D composed over the key, `omk::resolveOverlayPlanes` -> C (565, R16_UINT) and M (R8) uploaded, `overlay.frag` blending `C + world x M` and the colour fade over the full-precision world in `presImg_` - no readback of the 3D at all. The 2D stays 565-sourced (it is the game's art). Vulkan's overlay frames stay on the CPU (its present is a `vkCmdBlitImage`; an overlay pass there is not written); the hard-gated frames (the viewport item, SAVE GAME's thumbnail, the CPU mirror, the instruments, `--dump`) stay 565 on both. `OMK_WINDUMP=<path>` writes what the window got on the last frame (D3D12's back buffer) - `--dump` stays the software reference's RGB565. In `all = max` | `colordepth = 16\|32` / `--colordepth 16\|32` | **done 2026-10-18**; `engine: colour depth` |
| 22 | **Post-processing chain** (Direct3D 12): a display-size chain after the scene, SSAO and the upscaler and before the present pass (`bloom.frag`, `post.frag`); the 2D is laid over it afterwards (the overlay pass, or the CPU composite over `readback()`, which reads the chain's picture), so the interface is never bloomed, blurred or graded. The scene target stays RGBA8 - an FP16 one would change every scene pipeline, the MSAA resolve, the upscaler's input and the readback for light the scene shader never makes - and the headroom is made where it is used: the 8-bit picture to linear light (gamma 2.2) and EXPANDED by the tone curve's inverse shoulder (knee 0.5), so the curve maps it back onto itself exactly wherever nothing is added. **Bloom**: a bright pass (luma, or 0.6 of the brightest channel, over ~247/255 with a soft knee - so a pale sky does not glow), the Jimenez 13-tap downsample with a Karis average into six R11G11B10 levels, tent upsampling with each wider level at 0.6 of the one inside it, added at 0.25. **Colour grade** `cyber` (teal shadows, magenta-orange highlights, a mild S-curve, saturation 1.15), `warm`, `noir`, `filmic`: lift/gamma/gain per channel, an S-curve pivoted at 0.4 (a dark game), saturation, a split tone - no LUT. **Depth of field** on CINEMATIC frames only (a full-width letterbox strip shorter than the frame: conversations, cutscenes): a single-pass golden-angle gather (~48 taps, radius up to 1.2% of the height) from the depth buffer at its render size (MSAA: `post_ms.frag`), focused where the camera aims - the median depth about the strip's centre, where its target projects, floored at the eye-to-target distance, which is often a point in the air (143 units in the supermarket fight's opening shot). **HDR**: an scRGB (FP16, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`) flip-model swapchain only when the window's output reports ST.2084 (`IDXGIOutput6::GetDesc1`), else a line and SDR; paper white 200 nits, the shoulder's white at the display's peak over it, colour depth 32 implied; `OMK_HDR_FORCE=1` takes the path on any display. Built to the DXGI documentation and NOT seen on a real HDR monitor. Measured on an RX 9070 XT at 3840x2160 (`OMK_POST_TIMING=1`): bloom + grade 0.33 ms, with depth of field 0.55 ms. Vulkan says so and stays off. Everything off: byte-identical to the tree before it (`cmp`, three pictures) | `bloom = off\|on`, `dof = off\|on` (both in `all = max`); `colorgrade = off\|cyber\|warm\|noir\|filmic`, `hdr = off\|on` (by name only) / `--bloom`, `--dof`, `--colorgrade G`, `--hdr` | **done 2026-10-19**; `engine: post chain` |
| 23 | **Soft particles** (Direct3D 12): the effect sprites - `Render_SubmitSprites`' billboards: smoke, steam, the fires, the lamps' star glows; the frontend marks their additive and multiply batches `kDrawSoft` (`Draw::lit` bit 8) only for a renderer that answers `softParticlesLive()` - are drawn by `scene.frag` built -DOMK_SOFT (`scene_soft`, `scene_soft_mv` with the upscaler; the plain build even with ray tracing, whose root is a superset) with the depth bound READ-ONLY (a second DSV, `D3D12_DSV_FLAG_READ_ONLY_DEPTH|STENCIL`) and in `DEPTH_READ | PIXEL_SHADER_RESOURCE`, read through its existing view (`kSlotDepth`, a table at t0 space6 - 63 of the 64 root dwords with the upscaler and ray tracing); each fragment's colour is scaled by `saturate((sceneZ - fragZ) / 24)` in view units (the A, B of the projection ride in the Lights block), so an additive sprite adds less and a multiply one darkens less where it nears the opaque surface behind it. The depth is bound writable again by the next draw that writes it (the engine's order puts the sprites after the opaque). Not under MSAA (a multisampled depth): a line, and off; the upscaler turns MSAA off, so FSR/DLSS runs have it. In the street start it touches 0.11% of the frame - the steam against the wall and the wall lamps' glows, whose cores sit in the wall; the brazier fire stands clear of everything and is untouched. Subtle by nature: a sprite flush with a wall loses some of its brightness with its hard edge (24 units was chosen over 48 for that, by eye; `OMK_SOFT_RANGE=` to judge). In `all = max` | `softparticles = off\|on` / `--soft-particles` | **done 2026-10-19**; `engine: soft particles` |
| 24 | **Volumetric lamp light** (Direct3D 12): the lamps' light scattered in the air round them. `View::airLights`: up to 16 of the set's `.3DO` lamp records whose reach is in front of the camera and within 150 m of it, nearest first, the SKY FILLS (reach past 8000) left out - chosen for the camera, not the player as `View::lights` is, and only for a renderer that answers `volumetricsLive()`. `vol.frag` at HALF the render size: each pixel's eye ray to the opaque depth, and per lamp the stretch inside its glow sphere (half its record's outer reach) marched in 8 EQUAL ANGLES (equiangular sampling, Kulla & Fajardo 2012 - the inverse square with a core of 5% of the reach integrated exactly, the steps carrying only the window `(1 - r/R)^2`, a Henyey-Greenstein phase g 0.35 and the scene's linear fog), jittered by interleaved gradient noise; `volmix.frag` brings it up with a 4x4 joint-bilateral gather on the depth and ADDS it (one + one, half a level of dither where there is light) over `colour_` after the MSAA resolve and SSAO, before the upscaler and the post chain - so it is upscaled, bloomed and graded with the frame. No shadowing (the ray stops at the surface; the shadow map and the TLAS were not needed for a halo). MSAA: `vol_ms`/`volmix_ms` read the first sample. Density 0.0011, by eye: a warm halo round each lamp head and the wall beside it, the dusk sky round the post lamps a shade warmer - 8% of the street start brightened, nothing anywhere darker, every change inside the lamp's projected glow; with the earlier full reach the camera stood inside three lamps' glow and the whole frame went hazy, which is why the glow is half the reach. `OMK_VOL_TUNE=density,core,glow` to judge. RX 9070 XT at 3840x2160 (`OMK_VOL_TIMING=1`, `--nodelay`): 0.25-0.30 ms. In `all = max` | `volumetrics = off\|on` / `--volumetrics` | **done 2026-10-19**; `engine: volumetrics` |
| 25 | **Texture override packs**: replacement PNGs for the `.3DT` pages - `DIR/<group>/<model>/<MATERIAL>.png`, the material record's +0 name (`packFileNames`; a repeat takes `~<index>`) - at a whole multiple k of the original's size, uploaded by Vulkan and Direct3D 12 in the page's place. The workflow: (1) `python3 tools/texpack.py extract DIR` dumps all 2534 pages (or `--model NAME`); (2) `texpack.py upscale DIR DIR4 --factor 4 --tool realesrgan-ncnn-vulkan.exe [--model realesrgan-x4plus]` runs the external AI upscaler per file (`-i in -o out -n MODEL -s 4`), or with no `--tool` the built-in Lanczos-3 + mild unsharp (pure Python, a baseline only); the colour key is handled apart - key texels filled from their neighbours before, the original's key put back as pure black after (bilinear mask at 0.5); (3) `texpack.py check DIR4` lists what is overridden and what the engine will refuse; (4) `texturepack = DIR4`. The engine indexes the pack at start (each model directory's `.3DT` decoded, the PNG headers read), matches what the frame's pool holds by the original's PIXELS (FNV-1a of indices + palette - so the slot cache's cross-model hits take the right picture), decodes a PNG (`formats/png.cpp`, written here: inflate + unfilter, 8-bit RGB/RGBA) on first use. The UVs stay in the original's texel units: `scene.frag` divides by the image size over k (k in the cutout push word's bits 8.. - D3D12's root signature has no dword to spare), `rt.glsl` the same (k-1 in the record's texture word's bits 24..). Pure black (or alpha < 128) is the key; mips and filtering run on the replacement; the grey bank (ops 150/151) greys it by luma (`greyRgba`). The software reference, GLES, GL1 and the 3DS keep the original. A bad file falls back with a line. NOT in `all = max` (it names a directory) | `texturepack = DIR` / `--texturepack DIR` | **done 2026-10-19**; `engine: texture pack` |
| 26 | **Ray-traced reflections on the wet ground** (Direct3D 12, with `raytracing`): the GROUND - no material or gloss data exists (a material is an atlas page: Anekbah's `BATITR01`..`20` mix walls, floors and trims), so `o3de/floormesh.h` reads it off the geometry and the mesh names, measured over all 220 decors: a mesh with 60% of its opaque area facing up (-Y), single-sided, not blended (water is two-sided or blended), minus the names that pass that shape and are not a street - canal/basin bottoms (`FOND`, `BASSIN`), grass (`GREEN`, `HERBE`, `GAZON`), snow (`NG_`, `NEIGE`), roofs (`TOIT`), flames. Anekbah: 127 of 860 meshes (`YX*`, `X*`, `OP*` - 90% of its upward area); elsewhere `*ROUTE*`, `TROT*`, `*SOL*`. The frontend marks those runs `kDrawFloor` (bit 16; runs merge only alike); `scene.frag`'s -DOMK_RT build, for an upward face of one: puddles from three octaves of value noise over world XZ (45% wet; near-mirror, the albedo darkened) and damp elsewhere (rough, weak); ONE ray a pixel about the mirror direction in a roughness cone (stable per-pixel angle), against the set AND the bodies (passers-by, traffic); a hit = baked colour x texel (a body: texel x 0.4), a bright texel boosted 1.6x in the puddles (the bloom takes it), a damp ray clamped (no fireflies); a miss = the sky plane's mean colour (`View::skyMean`, the sky being 12.5x past any reach) fading to the fog at the horizon; distance fade over 2400 units; Schlick Fresnel (F0 0.06); the 2x2 quad mean without the helper lanes (which drew seams of sky along every edge). The FSR temporal resolve does the rest; the noise is fixed to the screen, not the frame. Cost unmeasurable at 4K FSR native on an RX 9070 XT (28.2-28.6 ms a frame either way, `--nodelay`, ~4.6 M reflection rays a frame). `OMK_RT_REFL_TUNE` for tuning by eye. Not done: glass/shop windows (not identifiable cleanly); a forced night (judged at the street start, dusk, and at road level beside the traffic) | `rtreflections = on` / `--rt-reflections` (in `all = max`) | **done 2026-10-19**; `engine: rt reflections` |
| 27 | **Frame generation** (the Direct3D 12 WINDOW; written here - the FidelityFX SDK in `deps/` ships only the upscaler, no frame interpolation): the scene pass writes its motion vectors whenever frame generation is on (`mvOn_`: the `_mv` shaders with or without an upscaler; no jitter without one; the camera-cut test and the bodies' previous poses with them). At every frame's end `fg_cap.frag` keeps the finished WORLD picture (`displaySource`: post chain, upscaler or scene, the supersample folded - before the present pass's quantisation and before the 2D) at the display size, with an RGBA16F image of its motion (display px) and view depth; two frames kept. At the present, `fg_splat.frag` pushes every pixel of frame N half-way back along its vector and every pixel of N-1 half-way forward along its own vector turned round (steady motion over two frames) into two R32_UINT heads by `imageAtomicMin` of (12-bit log depth << 20 \| the source's offset) - a depth-tested forward warp, deterministic; `fg_resolve.frag` takes the front-most surface: both frames at one depth = blended half and half; one in front = that frame's pixel, blended with the other frame's (bilinear) where its vector says the point was IF the other frame shows the same surface there (depth within ~13%, a like vector), else alone (a disocclusion); a crack takes the farthest neighbour's splat, only if its offset still lands on that surface (else a body's moved-off outline ghosts). The in-between picture goes through the same present pass and the SAME overlay as the real frame (the 2D is never interpolated: the frame's own interface over both; fades are in the world picture or the overlay's fade) and out at once; the real frame is composited behind it and presented half the measured interval later by a thread of the backend's on the swapchain's own HIGH-PRIORITY queue (so it need not wait behind the next frame's work; its own list, fence; the frame's thread settles it before touching the swapchain again; `OMK_WINDUMP`'s last frame goes out at once, a real one). Skipped: a first frame, a pair with a camera cut on either side (5 m / 30 degrees, a set released, a resize), a present with nothing new rendered, a frame composed on the CPU (colour depth 16 under the 2D). `--frames N` still renders and presents N real frames (60 real, 57 in-between at the street start), and the real frames are byte-identical to framegen off. Latency: the real frame half an interval late (~17 ms at 30 fps). Off under MSAA (a line). Quality (`OMK_FRAMEGEN_PROBE`: frames N-2 and N interpolated with doubled vectors against the real N-1, riding the street): mean error where the picture moves 3.9 warped against 9.4 for a plain blend (27% of the blend's with FSR native); the passers-by and the cars land where they were, no doubling; slight softening (the half-and-half of two frames, the bilinear fetch) and limbs mid-swing approximated linearly. Cost at 3840x2160 (RX 9070 XT, `OMK_FRAMEGEN_TIMING`): ~0.47 ms an in-between frame + 0.16 ms a frame for the capture. THE DISPLAY'S ROOM (the user's stutter, fixed 2026-10-19): an in-between frame goes out only while a frame's work (last present's return to this present's call: pacer, simulation, drawing) spans about two refreshes (on at 1.9, off under 1.7 refresh periods; `OMK_FRAMEGEN_ALWAYS` forces it, for the harness). Measured paced, 4K, the user's ini, 60 Hz, before: every in-between present blocked 22-25 ms on the v-sync, the deferred real frame (then timed from the in-between's return, now from the call) landed on the next one's heels, and the loop went from 21 ms to 57 ms - 17 real frames a second, the player visibly stepping; after: 21 ms and 0.8 ms of present, as with framegen off (no in-between frames there: none has a refresh to show it in); at `framerate = 30` the in-between frames run and the real rate holds ~29. Before that: it pays only when the display can show twice the rendered rate: on a 60 Hz display (this machine's reports 60) the two v-synced presents of a frame cost a refresh each, so a game rendering above 30 fps is held near 30 real frames + 30 in-between (by that reasoning and the 4K run's 32 ms present phase; not measured cleanly). In `all = max` | `framegen = on` / `--framegen` (in `all = max`) | **done 2026-10-19**; `engine: framegen` |

## On the GLES backend (2026-10-05) - and what the Vita can take

`backends/gles/glesrender.cpp` draws every GPU row, each measured by the same
check as Vulkan's through `gles_probe` (headless CGL on the Mac; skipped
elsewhere) and each shown to fail:

| row | how, on GLES2 | measured | Vita (vitaGL) |
|---|---|---|---|
| 0 MSAA | a multisampled colour+depth target resolved into the world target by a blit at `end()` | 4 of 4, 2.77% changed, 99.63% on an edge (Vulkan 2.7%); `engine: anti-aliasing` | **refused, said at start-up**: GLES2 has no multisampled renderbuffer, and vitaGL's MSAA is the DISPLAY's, a mode fixed at `vglInit` for every surface - not a target this backend can resolve. Supersampling is the Vita's anti-aliasing |
| 2 trilinear | `glGenerateMipmap` at upload, power-of-two textures (every one the engine repeats) | gradient 4.258 < 5.054 < 5.693 - Vulkan's to the second decimal; `engine: mipmaps` | yes |
| 2 anisotropy | `EXT_texture_filter_anisotropic`, with trilinear only | 16x on an M1 | **refused**: vitaGL answers a maximum of 1x |
| 6 mapped shadows | a depth pass from the light PACKED into RGBA8 (GLES2 promises no depth texture), plain and GPU-posed casters, Vulkan's slab and 3x3 PCF | 1026 shadowed at 159.5, tilted 187.5 / 131.5, both ways; `engine: mapped shadows` | yes, untested on a console |
| 7 per-pixel lighting | `scene.frag`'s law in `kSceneFragX`; a lit plain draw's normals in a buffer of their own (never streamed), a posed body's from its rest buffer | centre 51 = the law, spread 11 against 0, plain and posed; `engine: per-pixel lighting` | yes, untested on a console |
| 9 supersampling | the world target N times larger (halved, said, when the context cannot hold it), resolved by Vulkan's rule in `readback` AND in a present program of its own | 1x identical, 4x mean 15.6 of 765, energy 80%, present EXACT full-frame and letterboxed; `engine: supersampling` | yes - at 4x a 3840x2176 target, which the console's GPU and memory may not carry; a refused allocation halves the factor |

Rows 6 and 7 live in ENHANCED copies of the scene and posing programs
(`kSceneVertX`, `kPosedVertX`, `kSceneFragX`), linked IN PLACE of the default
two only when either is asked for (`glesSetEnhancedLighting`, before `init`),
so a default run builds, compiles and caches exactly what it did before; at
rest they draw the default picture byte for byte (`gles_probe`'s "enhanced
programs at rest", asserted by `engine: gles backend`'s failure count).

**The Vita's shader cache** (`scripts/vita-shader-cache.sh`) is keyed by each
shader's source, so the five new programs - the two enhanced ones, the
shadow pass's two and the supersample present - are NOT in the cached set. A
console with `libshacccg.suprx` compiles them at start-up; one without it
refuses the enhancement (never the renderer) and says so. A run with the
enhancements on, where the compiler is, then the script, carries them.

## Row 4 - what lifting the clip is actually worth

Worth writing down because it is the opposite of what the row promised, and
because it is the kind of claim this repo is supposed to measure rather than
assume. On Anekbah's main street from the apartment save:

| clip | mesh runs drawn | culled | frame vs unlimited, fog equalised |
|---|---|---|---|
| 25 m | 68 | 1564 | 16.78% of pixels differ |
| 50 m | — | — | 11.66% |
| 100 m | — | — | **0** |
| 200 m (the option's maximum) | 1264 | 368 | **0** |
| unlimited | 1632 | 0 | — |

So the enhancement buys 368 extra mesh runs at 200 m and **not one pixel**,
and the same is true at an address in each of the other three cities. The
sets are built so the option's own maximum already reaches everything in
them. What limits the view at 200 m is the FOG, which moves 17.7% of that
frame - and that is why the two are lifted together: lifting the clip while
leaving the fog would change nothing at all.

The row is still worth having: it is what a player who has turned the option
DOWN gets back, and it costs nothing when it is off.

## Row 3's other half - the INTEGER interface mode is NOT done

`uiscaling = linear` filters the stretch; it does not change WHERE the
interface lands. A classic integer mode - scale by the largest whole number
that fits and centre the result, so a 1280x960 window draws every interface
pixel as an exact 2x2 - needs something this port does not have: an OFFSET.
`ScreenComposer::scaleX(v) = v * dw / 640` is used for POSITIONS
(`scaleX(f.x + q->offsetX)`), for SIZES (`scaleX(it.w)`) and for both at once
(`col * scaleX(64)`, the tile map), across **43 call sites** in
`src/ui/screendraw.cpp`. A centring offset may be added to the first kind and
must not be added to the second, so the mode cannot be had by changing one
function - every site has to be classified. That is a step of its own and it
can break the interface a player uses, so it was not smuggled into this one.

It also only half makes sense: an interface OVERLAY has to line up with the
3D view underneath it, so centring the interface at a different scale from
the world would separate the two. The mode is worth having for the
full-screen menus and worth thinking about for the overlays.

## Not an enhancement, and it came out of this list: THE SHIMMER

Written down here because it was found while looking for enhancements and is
the opposite of one. Mesh flag `0x8000000` oscillates the vertex colour of
**233 set meshes** — the far skyline of every city, 132 of them in Lahoreh —
and the port decoded it into `Corner::phase` and then read that phase nowhere.
A defect, not a missing feature: fixed on 2026-09-09, on by default, in both
backends. `docs/ASSETS.md` 4c.

**The lesson for this file**: before building an enhancement, check whether the
game already does the thing and the port simply dropped it. That is cheaper
than any enhancement and it is what the original looks like.

## Not an enhancement either, and it is the second of the same shape: THE DITHER

`sub_4638C0` sets D3DRENDERSTATE 26, `DITHERENABLE`, to **1 on both of its
device arms** - the same function that established, in the line above it, that
the shipped renderer has no anti-aliasing and samples POINT (its SOFTWARE
arm; the hardware arm filters bilinear - ASSETS 4, corrected 2026-10-03, and
that is now the GPU default too). So dithering is
what the game does, and the port drawing hard 565 bands where the original
drew dithered noise was a fidelity defect, not a missing feature. Fixed
2026-09-09: on by default, both backends, one quantisation point
(`src/ui/surface.h`). `--no-dither` exists only so two frames can be laid side
by side; it is a comparison tool, not a setting.

**What is ported is the DECISION, and the pattern cannot be.** Direct3D
dithers inside the driver's conversion to the framebuffer format, so the
matrix belonged to whatever card the player had in 1999 and nothing in this
tree can reach it. The 4x4 ordered Bayer here is a RECONSTRUCTION, in the same
sense as the mirror's plane normal and the audio attenuation law, and it is
labelled one where it lives.

Two numbers, from `engine: dither`. Over a 4x4 block the dithered output
reconstructs its input to **0.491 of 255** where plain rounding carries the
whole rounding error at **2.047**, and it does that at a signed bias of
**-0.249** - the dither is centred, so it does not brighten the frame. On
Anekbah's camera 3 a real frame moves **132792 of 307200** pixels and its
distinct colours go **103 to 161**.

**And the first version of it was wrong in the way this file should record.**
The threshold ran 0..+7 rather than -8..+7. It still breaks the band - it
still spreads a block across two levels, which is the visible half of the job
- and it lifts every pixel by about half a step while doing it, so the whole
frame brightens. `quantise888` ROUNDS rather than truncating, so the dither it
wants is a signed perturbation ABOUT the rounding point, not a positive bias
added before one. Measured, the uncentred version reconstructs a block to
3.475 of 255 at a bias of +3.475 - worse than not dithering at all, and worse
in the one direction a still frame does not show, because a picture that is
uniformly slightly brighter looks like a picture.

## Rows 5 and 6 - the shadows

**One key with levels, and it is SUBORDINATE to the game's own option.**
`displayshadows` (row 5 of the options menu, `[Preferences]`) says whether
shadows exist at all; `shadowquality` says how they are drawn. With the option
off the enhancement draws nothing. `classic` is the default and must render
byte-identical to what ships now - that is the first of the three checks.

### 5, fitted - DONE 2026-09-09

The blob follows the ground. `Shadow_EmitBoneBlob` lays the model quad at
`floor - 1` with every corner sharing that y, so at a step the shadow is cut
off dead at the lip; fitted subdivides it **4x4** and lays each of the 25
vertices on the surface probed under IT. On the bank's entrance stairs the
player's own blob spreads **10.7 units** vertically - one 30 cm riser - where
classic spreads 0.00, and on the flat street both are 0.00.

The cost is paid by gathering, not by probing: `soupInBox` takes the
triangles under a body ONCE and the 25-vertex grids probe against those. That
is fewer city-wide scans per body than the unfitted path already did.

Two things declared with it:

* **This one is NOT backend-gated, which departs from the rule at the top of
  this file.** The change is to the geometry the port generates, not to how a
  backend rasterises it, and making the two backends build different geometry
  would destroy the property the renderer boundary rests on - that they draw
  the same picture from the same decisions, which `mirror pass` and
  `engine silhouette` measure at 0.998 and 0.995. It is off by default, so the
  software reference still draws what the original drew, which is what the
  rule protects.
* **A per-vertex drop is CLAMPED** to the blob's own half-width, so slopes up
  to 45 degrees are followed exactly and a blob overhanging a ledge does not
  stretch down the drop. This port's number, not the engine's.

**The stacking is NOT fixed, and it turned out not to be geometry's to fix.**
Ten blobs overlap under a standing character and each multiplies under
`dst x (1 - src)`, so the pool goes nearly black. Taking the MAXIMUM instead
of the product needs somewhere to accumulate coverage - a buffer or a stencil
- which is a backend feature and not a shape. Culling the blobs that lie
inside a larger one does not stand in for it either: the contained ones are
the shins, which are the DARKEST because they are nearest the ground, so
dropping them lightens exactly where a shadow should be strongest. Row 6 gets
it for nothing, because a shadow map is a visibility test rather than an
accumulation. A stencil pass on the Vulkan backend, blobs drawn darkest
first so "first wins" is "max wins", would also do it and would be properly
backend-gated - written down here rather than done.

Remainder: the CROWD's blob is still `Slider_PlaceShadow`'s single node
turned to the floor normal, under both qualities. It is already
ground-aligned, which is the fault fitted fixes, so it gains least; on a
stair it would still float.

### 6, mapped - DONE 2026-09-09

Built as designed: an optional light on `View`, a `castsShadow` flag on
`Draw`, `Renderer::shadowPass` defaulting to a NO-OP so the software reference
ignores the whole thing, a depth-only Vulkan pipeline into a 1024x1024 map, and
a 3x3 PCF lookup in `scene.frag`. The slab is ORTHOGRAPHIC and fitted to the
casters, so a fragment outside it is lit by definition and a characters-only
shadow cannot darken the far end of a street.

**The light is the set's own, and the data made that easy**: 153 of Anekbah's
155 `.3DO` lights point within 30 degrees of straight down - they are the
street lamps, at y -350 to -426 with a reach of 700-900 units. The pick is
`strongestLightAt`, `applyLights`'s own reach and falloff rules, asked at the
PLAYER. Standing under `LIGHT6` at (1954, -350, -3184) the shadow lands under
his feet, which is what a lamp overhead does.

Four things that had to be got right, three of them found by measuring:

* **`setTextures` RESETS the descriptor pool**, which frees every set
  allocated from it - the shadow's set 1 included. Allocated once at start-up
  it was destroyed by the game's first texture load, the fragment shader read
  a strength of 0, and nothing was ever drawn. The synthetic probe passed an
  EMPTY texture list, took the early-out and never reset, so it worked while
  the game did not: the bug lived in the difference between the two callers.
* **A `vec3` in a push-constant block is 16-BYTE ALIGNED.** `fogColour` had
  been packed at offset 76 in C++ against 80 in GLSL since the fog landed, and
  nobody saw it because the shipped fog colour is BLACK - the four misplaced
  bytes were zeros either way. Adding an `int` after it made it visible: the
  new field read padding and every character shadowed itself. A latent fault
  the enhancement flushed out.
* **A caster must not RECEIVE.** The engine's shadow only ever darkens the
  ground, and a map that also shadows the caster puts its own silhouette
  across a low-polygon character.
* **The slab is centred on the PLAYER, not on the cast's midpoint.** Eleven
  scattered bodies put that midpoint out in the road beyond every lamp's
  reach, so the light picked there was the one distant fill that reaches
  everywhere - 20 degrees below horizontal, whose shadow lands off the frame.

Declared: the slab radius (220 units) and the 1200-unit range that decides
which bodies are bounded are this port's numbers; a body outside the slab
casts nothing. `--world-vulkan` was added as a HARNESS - the world drawn
through an offscreen Vulkan renderer while the frame is presented on the CPU -
because `--vulkan` needs a real window and nothing GPU-only could otherwise be
measured headlessly.

### 6, as designed - where the light comes from

**Not an invented sun.** The port already reads the sets' light table
(`formats/light3do.*`): 4179 records of position, colour, two radii and an
intensity, with a direction the loader computes as `normalize(centre - pos)`.
Every one of `sub_4380B0`'s call sites is street life, so the lights that
light the crowd are the lights that should cast it. Where no light reaches a
body, fall back to straight down, which degenerates into the blob.

**Characters only, and this is a constraint rather than a saving.** A set is
shaded by a colour baked into every vertex (`ASSETS` 4c) and that colour
already contains the artists' shadows. A map that darkens the set from the set
fights lighting that is already there and double-darkens every corner. Casting
from actors and the crowd alone is also exactly what the game shadows.

**The boundary change is small**: an optional light on `View`, a `castsShadow`
flag on `Draw`. The Vulkan backend renders a depth pass from the light over
the casting draws and samples it; the software reference ignores both fields,
so PORTING A2's "the boundary itself moves 0 pixels" still holds for it.
Multi-pass has precedent - `drawWithMirror` already does a second full pass
with a GPU stencil - and the mirror is the thing to watch, because its
reflected pass has to sample the same map or reflections lose their shadows.

### The three checks

* `classic` renders byte-identical to the build before the option existed.
* ~~`fitted` and `classic` are IDENTICAL on flat ground~~ - **this was the
  wrong instrument and the first version of the check used it.** The classic
  blob is a four-triangle FAN with a centre vertex carrying the average of the
  four UV pairs and the fitted one is a grid with bilinear UVs, so the two
  sample the same disc slightly differently and differ by ~1500 pixels on
  perfectly flat ground. What is asserted instead is the geometric property
  the enhancement exists for: the VERTICAL SPREAD inside one blob, 0 for every
  classic blob by construction, and measured on the PLAYER's own blobs
  because the global maximum is dominated by a fixed staged body elsewhere in
  the city and would not move if his were wrong.
* `mapped` must **MOVE**. A body between a known set light and a wall,
  rendered at two positions, and the dark region translates the way the light
  predicts. A shadow verified in one still frame is not verified (CLAUDE.md
  1, "some errors are invisible at rest") - and the shadows already shipped
  one fault of exactly that family, a blob six metres from its owner that two
  green checks could not see.

## Row 7 - DONE 2026-09-09

Built as designed. The vertex carries its normal to the GPU, the frame's
nearest eight lights go into set 1 alongside the shadow map, and `scene.frag`
runs the same reach test, the same linear falloff and the same `-(N.L)` per
fragment. Measured: at the point directly under a light the shader matches the
law computed from the formula EXACTLY, and along a scanline across a lit quad
per pixel varies by 51 where per vertex varies by 0.

**TWO BASES, and the split is the one place this departs from transcription.**
A body the engine itself lights starts from BLACK - `sub_494E80` writes
`instance[+416]` into every runtime vertex colour and every site that sets
+416 sets it to 0, and the crowd models ship pure white, so there is no baked
light in them to lose. A body the engine never lights does NOT: HO1_FN carries
real baked shading, and black-plus-lamps threw it away and left him a
silhouette wherever no lamp reached, which is what the first version drew. So
the crowd is lit from black and every other character has the lights ADDED to
its baked colour.

Two other things worth keeping:

* **the push-constant alignment trap caught me a second time.** `lit` was put
  in front of `fogColour`, which is 16-byte aligned - so the colour moved from
  offset 80 to 96, the C++ struct and the GLSL block disagreed, `lit` was
  never 1 and the whole enhancement drew nothing. Same fault as row 6's, one
  field later, and the shaders now say out loud that a new scalar goes AFTER
  the vec3.
* **adding lamps to an already-bright body changes little.** The player's
  baked colour is near-saturated, so the visible gain is almost all on the
  crowd, whose shading IS the lights. Said here rather than implied by a
  screenshot.

**What it looks like** (`renders/lighting-perpixel.png`, made 2026-09-09; the
directory is gitignored, so regenerate it rather than expecting it in a
checkout):

    build/omk-play ../gamedata ../tables --save ../traces/save-appart.bin \
        --area 0 --stand 2052,0,-1151,180 --density 4 --frames 150 \
        --world-vulkan --lighting pervertex|perpixel --dump out.bin

Measured on that frame: **5712 of 480000 pixels** move, 1196 of them by two or
more luma steps - and **0 of them are on the player**. All of it is the crowd,
which is the honest shape of the enhancement: their shading IS the lights, so
sampling it finer changes them, while a body that already carries a
near-saturated baked colour barely moves when lamps are added to it.

**What it does NOT take account of, asked by a reader and measured**: the
EFFECTS. A fire, a vent, a neon sign emits sprites and never light - nothing
in `ASSETS` 3b's chain carries a light record and there is no dynamic light to
attach one to. The street lamps and the signs ARE accounted for, because the
`.3DO` light table was authored onto them; where a fire seems to light the
street that is a record the artist placed beside it, and it varies by set (the
Impasse's flame has one 17 units away, the temple's four have none nearer than
93). So nothing flickers: a fire's light, where it has one, is static.
`docs/ASSETS.md` 4c, `verify.py: effects and lights`.

Still open, and named in the row below as it was: the port applies reach and
falloff per BODY where the engine does them per MESH. The GPU path is now per
FRAGMENT, which is finer than either; the CPU path is unchanged.

## Row 7, as designed - per-pixel lighting

### What the engine's lighting IS, which bounds what "enhanced" can mean

Three facts, and they are narrower than they look:

* **a set has no dynamic light at all.** It is shaded by a colour baked into
  every vertex, and 38.9% of set vertices are not grey (ASSETS 4c);
* **the `.3DO` light table is real and authored** - 4179 records over 216
  models, each with a position, a colour, an intensity, an inner and an outer
  radius, and a direction the loader computes as `normalize(centre - pos)`;
* **the receivers are narrow.** All EIGHT call sites of `sub_4380B0` are in
  `18_d3d.c`, the street-life module, so the lights fall on the procedural
  crowd and the traffic and on nothing else. The player and every staged actor
  are lit by their baked vertex colour alone. The law is `-(N.L)` over a linear
  falloff between the two radii, through the engine's `(t * c) >> 8` ramp.

Two further paths are DEAD and not worth chasing: the per-object live light
gated on mesh flag `0x8` is carried by **0 of 15720** shipped meshes, and the
environment-map stage is never given a texture (ASSETS 4c).

### The enhancement, and why it is the most defensible one left

**Evaluate the same law per FRAGMENT, and let every character receive it.**

Neither half invents anything. Today the falloff and the dot product are
computed once per vertex and interpolated across the triangle; a character's
thigh is one or two quads across, so a lamp's pool bends over his leg in flat
facets and pops as a vertex crosses the falloff boundary. Per pixel is the
same arithmetic sampled finely enough to stop showing the tessellation.

Letting the player and the staged actors receive IS a deviation from the
original and must be labelled as one - but it is the same lights under the
same rules, and it settles the oddity that a passer-by is lit by a lamp the
player standing beside them is not.

Most of the machinery exists already. `Corner` carries the vertex normal
through the pose (read for the crowd's lighting on 2026-09-05), and row 6 has
just added set 1 - a per-frame descriptor set with a uniform buffer - which is
where a small light list would go.

Also to fix while there, and it is a FIDELITY item rather than an enhancement:
the port applies the reach and the falloff per BODY where the engine does them
per MESH, which is a declared deviation of `o3de/vertexlight.h`.

### The check

Sharper than a pixel count, and it writes itself: per pixel and per vertex must
**agree AT the vertices** and **differ BETWEEN them**. Sample the fragment
colour exactly at a vertex position and require it to match the per-vertex
value; sample the middle of a long triangle spanning a falloff boundary and
require it to differ. That measures "the same law, sampled finer" and fails on
any change to the law itself.

Then the motion test, as everywhere else here: a body walking out through a
lamp's outer radius must dim CONTINUOUSLY rather than in steps.

## Row 8 - lighting the sets, and why it is held back

The obvious next thought, and the lamps are sitting in the set's own file. The
problem is that the baked vertex colour **is** the artists' lighting and
already contains those pools, so adding the lights on top does not enhance the
picture, it doubles it - and there is no principled way to subtract what the
painter put in. It would be a look, chosen here, replacing the one the game
shipped. Kept as a row so nobody has to rediscover the reason; if it is ever
built it must say plainly that it overrides authored art.

Bloom on the neon and a coloured fog are the same shape: they would flatter
the game and have no data behind them.

## `all = max` / `--enhance-all`

One switch for the whole section, so nobody has to know what the list
currently is. The two the DEVICE caps - MSAA and anisotropy - are asked for at
their largest defined value and the backend reduces what it cannot meet and
says so, which is what "max available" means here; the rest have a fixed top.
The tops live in `settings.h` (`kMaxAntiAliasing` and friends) so the flag and
the ini key cannot drift apart from each other or from the parsers.

`verify.py: enhance all` is the guard that matters, because **nothing about a
forgotten enhancement is red**: it just stays off while the report says
everything is on. So the check compares the two directly - every key
`settings.cpp` reads under `[Enhancements]` except `all` itself must be driven
by `applyMaxEnhancements` - and is shown to fail by adding a key that is not.
It also asserts the precedence in the direction that matters: `all = max` with
`anisotropy = 4` gives 4, so a config still means what it says.

**Supersampling is NOT in it (2026-10-01, the reader).** At 4K on an M3,
everything else together held 120 fps (the vsync cap) and supersampling alone
took it to 8.6, then to 21.5 once its resolve moved to the GPU
(`todo/optimization.md` "The 4K benchmark"): 4x4 on top of 8x MSAA is ~128
samples a pixel for edges that 4K pixels and MSAA already smooth. So it is
turned on by name only - `supersampling = N` or `--ssaa N`, which beside
`all = max` still gives N. `verify.py: enhance all` names it as the one left
out and asserts both halves; putting it back in `applyMaxEnhancements` turns
the check red.

## Row 9 - supersampling, DONE 2026-09-09

`--ssaa 1|2|4`. The Vulkan attachments are made N times larger each way, the
render area and the viewport with them, and `readback()` averages each NxN
block down. **The resolve happens on the 8-bit side, before the one 565
quantisation** - averaging four 565 values would quantise four times and then
average the error, which is most of the point thrown away. The present blit
takes `VK_FILTER_LINEAR` only above 1, so at 1 nothing changes.

**Why it is not MSAA over again**, which is the question the row exists to
answer: MSAA samples geometry EDGES. This game's aliasing is mostly texture -
256x256 atlases sampled POINT - and CUTOUT, where the silhouette of a grille
or a railing is a colour key INSIDE a triangle, which is a place MSAA never
looks. Supersampling reaches both.

Measured on Anekbah's own camera 3: `--ssaa 1` is BIT-IDENTICAL to no flag at
all (0 pixels), 4x moves 26021, and the frame goes from **95 distinct colours
to 138** - the signature of a resolve rather than a shift, since averaging
makes values that were in neither sample.

**Not asserted, and deliberately**: that it looks less aliased. Edge energy
over a texture-heavy set falls about 1%, because most of the high-frequency
content there is the artists' texture and this averages that too. The eye
settles whether it looks better; the check settles that it resolves.

Three faults on the way, all mine and all the same shape - a change applied in
one place and not its partner:

* the **render area** stayed at the output size inside a larger attachment, so
  everything outside it was never cleared and the resolve averaged
  uninitialised memory into magenta stripes;
* the **readback's resolve never landed at all** - it was in an edit batch
  whose other anchor failed, so the whole batch was refused while the rest of
  the plumbing went in. The symptom was a black frame, because reading the
  first `w*h` pixels of a 2x-wide buffer walks the top quarter of the picture,
  which on that set is ceiling;
* `presentSurface` uploads a finished picture at the OUTPUT size into the
  top-left of the attachment, so the present blit had to be told which region
  is live rather than assuming the whole target.

Known limit of 1 and 2, to be judged by eye: the sets sample sub-rectangles
of shared atlases (the Anekbah signs), and a filter reaches half a texel past
a rectangle's edge into its neighbour. Nothing per draw says where the
rectangle ends, so it is not clamped.
