# Handoff — the CLASSIC MAC (PowerPC) PORT (2026-10-03)

**Read this first to pick up the PowerPC Mac work.**
[`classic-mac-port-1999.md`](classic-mac-port-1999.md) is the plan and the
full record (the 1999 what-if, every finding with its evidence, sections 3a-i
to 3d-ii); this file is the state, the recipes and what to do next. It is
NOT a modern macOS port - OMK already runs on today's Macs.

---

## 1. Where it stands

* **`omk-play` RUNS ON MAC OS 9 AND TIGER through the Carbon frontend**
  (2026-10-03, `e08bc23`, plan 3d-iv): 30 frames of Anekbah's street start
  BYTE-IDENTICAL to the host on both. Software renderer, AUDIO through the
  Sound Manager (`8b536ad`: 72 s played in a 75 s run on Tiger; choppy on
  emulated OS 9, where a frame takes seconds), and nobody has driven it by
  hand. Getting there fixed three big-endian bugs in
  the viewer (the PowerPC Darwin build had them too), worked round Retro68's
  linker leaving weak `.bss` objects without storage, and `%zu`.
* **THE ENGINE RUNS ON MAC OS 9** (2026-10-03, `aa7aa43`, plan 3d-iii): the
  headless boot (`tools/omk.cpp`) as ONE Carbon binary (`make classic`),
  run on Mac OS 9.2 and on Tiger - its boot dump BYTE-IDENTICAL to the
  Mac's. It took `int32_t`-is-`long` fixes, a File Manager arm in `DataFs`
  with `omk::hostPath()` (HFS paths), no threads, and NO C++ STREAMS in
  `src/` (a Retro68 linker fault, trap below). `verify.py: engine: classic
  build` keeps it building and stream-free.
* **The viewer's game code reaches the host only through `omk::Frontend`**
  (the gateway, `12d77ec`), so a Carbon frontend can replace SDL.
* **OMK runs on Mac OS X 10.4 (Tiger) on PowerPC**, in QEMU (ppcosxkvm,
  emulated Radeon 9700): the films with sound, the menu, the intro, and the
  street **at 6-10 fps through the fixed-function OpenGL 1.x backend**
  (`engine/backends/gl1/`), against ~1 fps in software. Colours and sound
  confirmed by the reader.
* **Big-endian is done.** `formats/le.h` (`loadLE`, `writeLE`) at every raw
  read; every tool a `verify.py` check runs, 1928 distinct calls over 146
  tools, replayed on PowerPC: **1927 identical** to the Mac (the last is
  directory order). On the Mac all 1928 byte-identical before and after.
* **One Carbon binary runs on Mac OS 9.2.1 and on Tiger** (a Retro68 test
  program, 3d-i): C++20, exceptions and RTTI work; no `std::thread`, no
  `std::filesystem` (does not link), `%zu` prints "zu".
* **The fixed-function backend** follows the original's D3D path (one draw
  per bucket, every state cached, texture by `key & 0x3F`, 16-bit textures,
  the original's blends) and its CPU side: vertices transformed, clipped
  (near plane AND screen edges) and culled on the CPU, handed over already
  projected; fog per vertex. Against the software reference on the M3:
  coverage 0.9932, 153 px - `verify.py: engine: gl1 backend`.
* **ASSETS 4 corrected (2026-10-03)**: on a 3D card (driver mode 0, the
  default) the original FILTERS bilinear, no mipmaps; POINT is the software
  devices'. `render states` asserts both arms. The DEFAULT followed the
  same day, the reader's decision: bilinear on every GPU backend (4, item 1).

Commits, oldest first: `9cabf0e` PowerPC build, `0a4a33f` le.h,
`16f2ea1` every tool on PowerPC, `8287f8b` one Carbon binary, `2a7e025` the
game on Tiger, `db78df4` the emulator's colour bug, `523d530`
`AUDIO_F32SYS`, `d50cba8` the gl1 backend, `80c83f4` its window title,
`8dad555` screen-edge clip, `f826df6` its check, `86ef012` the filtering
correction. All pushed.

---

## 2. The tools - outside the repo, never named by it

Everything lives in an APFS sparse bundle on the external SSD (exFAT's
128 KB clusters made sources cost 10x). Its own `README.md` says what is
where and why; the build scripts in its `scripts/` are re-runnable.

    hdiutil attach /Volumes/Crucial_X8/decomp/tools/omk-devtools.sparsebundle
    # -> /Volumes/omk-devtools (must mount under THAT name: binaries refer to it)

| what | where on the volume |
|---|---|
| PowerPC cross-compiler (GCC 14.4, `powerpc-apple-darwin8`) | `toolchains/ppc-darwin8/bin` |
| Retro68 (classic Mac OS / Carbon, Universal Interfaces) | `toolchains/retro68/bin` |
| SDL 2.0.3 for Tiger (built IN Tiger, its Cocoa half is ObjC) | `sdl2-tiger-g3/` - generic PowerPC, runs on a G3 (2026-10-04; the build tree is Tiger's `~/src/panther-sdl2-g3`); `sdl2-tiger/` is the first build, `-maltivec`, G4 only |
| ppcosxkvm (Tiger VM, **locally patched** - see 5) | `emulators/ppcosxkvm/` |
| QEMU with Screamer sound, for OS 9 | `emulators/qemu-screamer/`, `scripts/run-os9.sh` |
| the 10.4u SDK (cross sysroot), MPW GM, Xcode 2.5 dmg | `sdk/` |
| the replay harness for the tool comparison | `work/bereplay.py` |
| the upstream ppcosxkvm report, DRAFTED NOT SENT | `upstream/ppcosxkvm-txo-endian/` |

OS install images: `/Volumes/Crucial_X8/decomp/tools/os-images/`.

---

## 3. Recipes

**Start Tiger** - DETACHED, or a host background command is killed at 2 h:

    cd /Volumes/omk-devtools/emulators/ppcosxkvm
    nohup ./ppcosx --ssh-port 2222 --ram 2048 > /tmp/tiger.log 2>&1 < /dev/null & disown
    ssh -F /Volumes/omk-devtools/vm/ssh/config tiger      # user qemudev, key auth

Shut it down from inside (`sudo shutdown -h now`), not by closing the window.

**Build** (from `engine/`; `ppc-darwin.mk` is this port's own makefile -
the shared `Makefile` is untouched by all of it):

    PATH=/Volumes/omk-devtools/toolchains/ppc-darwin8/bin:$PATH
    make -f ppc-darwin.mk                                   # 196 tools, build/ppc-darwin8/
    make -f ppc-darwin.mk SDL2_PREFIX=/Volumes/omk-devtools/sdl2-tiger-g3 play   # omk-play, gl1, any G3
    # PLAY_GPU=none for software only; OMK_GL1=0 at run time to compare
    # the same, for the MAC (what the check builds):
    make -f ppc-darwin.mk PPC_CXX=c++ PPCFLAGS=-ffp-contract=off LDFLAGS= OUT=build/host-gl1 \
         SDL2_PREFIX=/opt/homebrew PLAY_LIBS="$(pkg-config --libs sdl2) -framework OpenGL" play

**Run the game in Tiger** - it must go through `open` on the bundle
`~/omk/OMKPlay.app` (a launcher script): started over SSH it is not in the
desktop session, gets no window and exits silently. Data in `~/omk/fr`,
tables and traces in `~/omk/repo`, binaries in `~/omk/bin`. Extra
arguments go, one line, in `~/omk/play.args` (e.g. the street start
`--save repo/traces/save-appart.bin --area 0 --stand 1804,0,-6890,336 --nofmv`);
the log is `~/omk/play.log`; `screencapture -x` over SSH takes a screenshot.

**Copy into Tiger with `scp -O`** (or `ditto -c -k --sequesterRsrc` for
anything with a resource fork). Never a macOS `tar` of a sparse file: it
goes as a pax sparse entry and Tiger's GNU tar 1.14 unpacks a DIRECTORY.

**The engine for OS 9 / Carbon** (from `engine/`; `retro68 =` in `omk.conf`
lets `verify.py` find the toolchain too):

    make classic RETRO68=/Volumes/omk-devtools/toolchains/retro68   # build/classic/OMKBoot.APPL

`make classic` also builds `OMKPlay` (the viewer); add
`-DOMK_CLASSIC_TOOLS="ped_probe;..."` to the cmake call for tool apps
(`OMKTool_<name>`). **OMKPlay on Tiger**: its folder with `OMKPlay` and
`omk.args` (one argument a line: `Macintosh HD:Users:qemudev:omk:fr`,
`Macintosh HD:Users:qemudev:omk:repo:tables`, `--save`, `...:traces:
save-appart.bin`, `--area`, `0`, `--stand`, `1804,0,-6890,336`, `--nofmv`,
and `--frames 30 --res 640x480 --dump cplay.bin` for a comparison), launched
with `open ./OMKPlay` (LaunchServices puts it in the desktop session;
`LaunchCFMApp` from SSH gives a windowless run). Compare against the host
built by `ppc-darwin.mk` with `PPC_CXX=c++ PPCFLAGS=-ffp-contract=off
PLAY_GPU=none` - not `build/omk-play`, which contracts. **On OS 9**:
`untitled:omk` holds the data (595 MB - the boot's three folders plus
FONTS, I2D, ANIMS, SOUNDS, MAP2D, RADAR, TRAJECTOIRES and, since 2026-10-04,
TRACKS - without it the game has no music), `save-appart.bin`,
`OMKPlay` and `omkplay.args`; copy the last two into Startup Items (the
args as `omk.args`), boot, wait ~3 minutes, stop QEMU, read `play.bin` and
`omk-out.txt`, empty Startup Items.

**OpenGL in the Carbon build** comes with `make classic` when the tools
volume has its two inputs - `sdk/MacOSX10.4u.sdk` (AGL's and OpenGL's
headers) and `sdk/os9-opengl/OpenGLLibrary`, Mac OS 9's library WITH its
resource fork, which `MakeImport` turns into the import stub (CMake cache
variables `OMK_MACOS_SDK` / `OMK_OPENGL_PEF` relocate them). Without them
OMKPlay is the software build, and `engine: classic build` says which. The
log names the path: `display: an AGL context on the window (accelerated)`
and `gl1: ATI Radeon 9700 OpenGL Engine, OpenGL 1.5 ...`.

`OMKBoot` reads its arguments from `omk.args` beside it, one per line, HFS
paths (`untitled:omk:fr`, `NOFMV`, `--tables`, `untitled:omk:tables`,
`--dump`, `boot.bin`), and writes `omk-out.txt`. **On Tiger**: `ditto -c -k
--sequesterRsrc` a folder holding it, `scp -O`, `ditto -x -k`, then
`/System/Library/Frameworks/Carbon.framework/Versions/A/Support/LaunchCFMApp
./OMKBoot` from its folder; arguments as `Macintosh HD:Users:qemudev:omk:fr`
etc. **On OS 9**: the data is already on its disk (`untitled:omk:fr` - IAM,
SCPTDATA, MESHES - and `untitled:omk:tables`, with `OMKBoot` and its
`omk.args` in `untitled:omk`); copy the app and `omk.args` into
`System Folder:Startup Items`, boot, wait ~3 minutes, stop QEMU, mount, read
- and take the app OUT of Startup Items again, or it runs at every boot.
A run WITHOUT `--frames` (audio on) never ends by itself there: put
`omk.quit` beside the app holding a number of seconds, and it quits after
that long and writes its closing lines (`audio: Sound Manager ... played`).
On Tiger, `osascript -e 'tell application "OMKPlay" to quit'` does it.

**Mac OS 9**: no shell. Put a program in the OS 9 disk's
`System Folder/Startup Items` (mount `vm/macos9.img` on the Mac with
`hdiutil attach -imagekey diskimage-class=CRawDiskImage`), boot with
`run-os9.sh`, have it write a file and call `FlushVol`, stop QEMU, read the
file off the image.

---

## 3d. THE GL1 STRAIGHT PRESENT, RUN ON TIGER (2026-10-05) - and the iBook

`a98ca45` (`todo/cpu-vs-original.md` tier B): the AGL build draws the world
at its letterbox place in the window's back buffer and SWAPS it - no
readback, no CPU composite, no `glDrawPixels` - the interface blended over it
in fixed function on a frame that has one, the DRIVER's dither (the
reader's choice). `--gl1-composite` in `omk.args` keeps the old path.
Measured on the emulated Tiger (the street start, 640x480, `OMKPlay` in
`~/omk/cplay`, logs `out-direct.txt` / `out-composite.txt` there):

| present | sim+draw | readback | present | a frame |
|---|---|---|---|---|
| composite | ~47 ms | ~74 | ~95 | ~216 ms (4.6 fps) |
| straight | ~43 ms | 0 | ~0.7 | ~44 ms (~23 fps) |

EMULATED timing - the reader ordered an **iBook G3 500 MHz, 640 MB** (ATI
Rage Mobility 128, 8 MB; OS 9.2 and Tiger) to measure on: its numbers
replace these. (`screencapture` over SSH writes nothing, and the host may
not script the emulator's window - a person looks.)

**The OVERLAY, watched by the reader on Tiger (2026-10-05)**: the supermarket
fight (`--fight-supermarket`) - *"The UI was showing correctly"*. (Kay'l dies
at once there: `save-appart.bin` gives him Vie 10 and the harness presses
nothing; `--fight-health` for a longer fight.) Its first version re-sent the
whole band of interface rows every frame - `present` 41-58 ms an overlay frame
on the emulated Tiger; now a row keeps its place in the texture and is sent
only when its 565 row or mask row changed (a hash a row, the GLES
overlay's): the subtitle's frames **45-52 -> 2.7-4.6 ms**, the gauges ~42 ->
26. A FADE folded into the texture changed every row every frame (25-31 ms);
since 2026-10-05 it is a SECOND QUAD, untextured over the window at the fade's
colour and weight, `(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` - `dst*(1-f) +
fade*f`, the GLES shader's law done by the blender: the same fight, same
frames (KO at 423, replay out at 511), `present` over the fade windows **26.0 /
25.0 / 31.0 -> 9.6 / 3.0 / 3.6 ms** (means of 60 frames; the first window
also carries the KO's gauges). Watched by the reader: see below.
Not run on OS 9 (its emulation has no 3D card).

## 3e. MAC OS X 10.2 JAGUAR - the reader's real PowerPC Mac (2026-10-09)

The reader ran the SDL `omk-play` (`ppc-darwin.mk`, Tiger target) from a
terminal on a real Mac under **Mac OS X 10.2**: `dyld: can't open library:
/usr/lib/libiconv.2.dylib`. Jaguar has no libiconv (Panther and Tiger do);
SDL 2.0.3's `SDL_iconv` imports it. It is only the first missing piece: the
Mach-O build targets 10.4 (`-mmacosx-version-min=10.4`, GCC 14's static
runtime against Tiger's libSystem, a Cocoa SDL port for 10.3+), so that
binary is not for Jaguar. The route for 10.2 is the CARBON `OMKPlay` (CFM,
runs on OS 9 and Tiger; Carbon runs on every Mac OS X) - handed over as
`engine/build/classic/jaguar/` (`OMKPlay` with its fork as `._OMKPlay` on
the reader's FAT32 stick, `OMKPlay.bin` MacBinary as the fallback, both
`make classic` of 2026-10-09 at `4b9d27d` + the profile fix, AGL inside;
`omk.args` with the relative paths `fr` / `tables`). **It RUNS on the
reader's iBook G3 under 10.2** (2026-10-09) - but "the UI are extremely slow
(even the city is faster)". Cause read in the code: a frame the world is not
swapped straight for (the menus, the films, a composite) went through
`CarbonFrontend::presentGL` as ONE `glDrawPixels` of the whole frame with
`glPixelZoom` - a software path in the period's drivers - while the city's
world is swapped straight and only its interface rows go up as a texture.
Fixed the same day: `presentGL` uploads the frame as RGB565 into a 16-bit
texture, only the runs of rows whose hash changed, and draws one quad.
Built and `engine: classic build` green; NOT yet run on any machine - the
reader's iBook is the test.

**WHITE SCREENS IN CUTSCENES, never in adventure mode** (the reader, same
machine, same day, on the build BEFORE the texture present). Not diagnosed.
Ruled out by reading: the fade colour (normalised to 0..1 in
`playframe_screens_huds.cpp`). Candidates: a frame forced onto the CPU
composite (`keep` = `cpu mirror`, `screen`...), whose `readback()` copies the
back buffer through a texture - on the Rage 128 that copy may fail and the
frame show the texture's undefined content; or a refused straight/overlay
present. The AGL build now LOGS its present path where it changes (`gl1
present: <straight | overlay (why) | why> from frame N`, and a one-time
`REFUSED` line), beside the existing `gl1: first readback ... centre pixel`
line: the next `omk-out.txt` with a white cutscene answers which.

## 3f. FASTER ON THE iBOOK - the reader's ask of 2026-10-09, IN STEPS

*"The current version is a good start but I think it could be optimized a
bit more (cpu/gpu). Could you, comparing with the original and also with
your own ideas, find ways to optimize it more?"* No iBook figure exists yet,
so the ranking comes from the host GL1 build (`verify.py: engine: gl1
backend`'s recipe, `OUT=build/host-gl1`) SAMPLED on the M1 (macOS `sample`,
Anekbah's street start, uncapped): of the main thread's work outside the
host-only readback and SDL blits, the GL1 `submit` and its vector inserts
were ~650 samples, the body posing (`composePoseAt`, `applyPose`,
`applyLights`) ~360, the draw lists ~50. On a G3 the submit weighs more:
it divided 6 times a triangle and 3 a vertex, and the 750 divides in 17-31
cycles without pipelining. The iBook's own split will come from the log's
`frame N sections (ms, mean of 60)` line, printed in every build.

**Step 1 - DONE 2026-10-09** (`gl1render.cpp`, `backends/classic/CMakeLists.txt`):
- **One draw call a run of equal state**, the original's mechanism (one
  `DrawPrimitive` a non-empty bucket, `Render_FlushBuckets`): the vertices
  are already clip-space with the fog folded into their colours, so only
  blend, alpha test and texture divide a batch. Street: **89 draws a pass
  -> 24 draw calls** (`gl1: 60 passes drew 5376 draws in 1466 draw calls`).
- **The wholly-inside triangle takes no clip**: each corner's outcode is
  the clip's own test, so all-inside comes out unchanged and all-outside-
  one-plane empty, decided without running the five clips.
- **No division**: the cull is the view-space determinant's sign, the
  position `(x/th, y/tv, z - 2 near, z)`; the fog's range a reciprocal.
- **One interleaved array** pushed per vertex (was four `insert`s of an
  initializer list - out-of-line calls in the sample).
- **The mirror on the card** (`drawMirrorScene`, the GLES/original order:
  reflection first, the room over it, no clear). Before, a mirror in view
  ran `drawWithMirror`'s CPU fallback - three passes, three readbacks, a
  composite, and the frame sent to the CPU present ("cpu mirror"): Kay'l's
  bathroom mirror, in the apartment where the opening cutscenes play.
  Probed on the host (scratch probe, Aapkayl, two cameras on `AP_mirror`):
  native against the fallback **68 and 12 pixels of 307200 differ by > 24**,
  the reflection ~25000 pixels; laid side by side, the same picture.
- **`-mtune=750`**: scheduled for the G3, instruction set unchanged.
- Measured on the M1: `world begin..end (submit, GL)` **0.75 -> 0.44 ms**
  (twice each); the street frame differs from the old one in 3 pixels.
  `engine: gl1 backend` green (coverage 0.9945, 145 px > 24 - it was
  0.9932 / 153), `engine: classic build` green, the Tiger GCC compiles the
  file clean. NOT yet run on the iBook.

**THE iBOOK'S FIRST MEASUREMENTS (2026-10-09, the 13:54 build, the reader's
run from the new game through the intro to the street, 3099 frames; the log
kept as `omk-out-1354.txt` on the reader's stick).** The card is an **ATi
Rage 128 Pro, OpenGL 1.1 ATI-1.3.28**. Means of 60 frames:
- **the start menu**: `present` **220-310 ms** a frame (`presentGL`'s
  RGB565 texture) plus `screens, hud` 52-175 ms (the menu's composing) -
  "the UI extremely slow", NOT fixed by the texture present;
- **a subtitle or a fade over the world** (`gl1 present: overlay`):
  `present` **25-57 ms**, against 0.1-3 straight;
- **the intro's scenes**: sim+draw 23-45 ms - `staged bodies` 8-20,
  `submit` 7-11, `world begin, set` a steady ~8 even with 7 draws a pass
  (on the emulated Tiger's street it is 0.2-0.5: not yet explained);
- **the street**: sim+draw 165-200 ms (~5 fps) - `submit` **65-99**,
  `pedestrians, traffic` **34-44**, `scripted motion: meshes placed` 13,
  `staged bodies` 14;
- one-off stalls: `audio` 95-137 (a voice line read and decoded on the
  main thread - no threads on the classic build), `game frame` 123-199
  (area loads), `controller` 81.
- the batching saved nothing in the intro (420 draws, 420 calls - every
  draw there a different texture); no `REFUSED`, no readback, no mirror
  line in the whole run, so no frame was ever composed on the CPU.

**Step 1b - DONE 2026-10-09, NOT yet run on the iBook**: measure what the
log could not split, and stop guessing the upload format.
- **`CarbonFrontend::probeUploads`**, run once when the AGL context is
  made: a 640x480 frame sent AND drawn with `glFinish` inside the time (a
  driver may defer an upload to the texture's first use), best of four, in
  RGB565, ARGB1555 and ARGB8888 (BGRA 8-8-8-8-REV, Mac OS X's own layout),
  and the overlay's RGBA bytes against ARGB8888; the presents then use the
  fastest the driver did not refuse. It logs the drawable's bits (thousands
  or millions of colours). On the emulated Tiger (Radeon 9700): **RGB565
  13.4 ms, ARGB1555 7.9, ARGB8888 7.3 -> ARGB8888; overlay bytes 7.5,
  ARGB8888 5.5 -> ARGB8888**.
- the overlay's changed rows sent in RUNS (one call a run, not a row);
- profiler zones `gl1: draw call` (the driver's share of submit),
  `gl1: overlay rows`, `gl1: swap`, `present: rows`, `present: swap`;
- the profiler looks for its `.ctl` every 30th frame while running (it
  opened a file every frame - on the iBook, from FAT32, inside the frame).
- Run on the emulated Tiger (the street, 240 frames): runs, exit 0,
  sim+draw ~40 ms as before. `engine: classic build`, `engine: gl1
  backend`, `engine: profiler` (3) green. The reader's stick now has the
  build and `--profile omk.prof` in `omk.args` (the old one beside it as
  `omk.args.old`); `python3 tools/omkprof.py omk.prof` reads the capture.
  The VM shuts down without a password by
  `osascript -e 'tell application "System Events" to shut down'`.

**What is next, ranked by what it should save on the G3 (each to be
confirmed by the iBook's `sections` line before it is started):**
2. **The bodies posed per VERTEX, not per corner** - the original's
   `sub_4947F0` transforms each unique vertex once (200 in PSH_FN against
   1143 corners); the port's `applyPose` turns position AND normal per
   corner, then `applyLights`. Or GL1 poses them itself (`posesBodies`):
   the bone folded into the camera, one 3x4 a mesh, so a body corner costs
   what a set corner costs. The second cost in the sample, ~360.
3. **The set transformed per VERTEX** (`Render_SubmitMesh` into its 75000 x
   48 pool): Anekbah is 139245 corners over 63079 vertices, 2.2 a vertex.
   Needs `cornerVertex`, which `dropCharacterArrays` frees for a set today
   (~0.5 MB in Anekbah, inside the 64 MB budget).
4. **The display at 16 bits** - no code: the original ran a 640x480 16-bit
   device, and a Rage 128 fills a 16-bit buffer about twice as fast. The
   reader can try "Thousands" in the Displays panel; FULL SCREEN at 640x480
   (AGL's own, as the original's DirectDraw exclusive mode) would be the
   code step after it.

## 4. What is next, in the order proposed

1. ~~**The filtering DEFAULT**~~ - **DONE 2026-10-03**, the reader's
   decision ("the default should be what the original does"): every GPU
   backend, GL1 included, defaults to bilinear without mips (what a 3D card
   drew), the software reference stays POINT (the software devices). GL1 and
   GLES learned bilinear for it (`f2875ee`), the default flipped in
   `c7a5443`; `--filter nearest` gives the old picture. LOOKED at in Tiger
   the same day: the street start through the emulated Radeon 9700,
   bilinear, still 10 fps, no artefact seen in one still.
2. ~~**The engine on OS 9, headless**~~ (`aa7aa43`) and ~~**the Carbon
   frontend**~~ (`e08bc23`) - **DONE 2026-10-03**, above. **NEXT, in order**:
   (a) PLAY it by hand on Tiger and OS 9 - keys, the menu, walking; the key
   map is untested by a person; **2026-10-04, first look by the reader on Tiger**: the SDL
   build through the GL1 backend booted through the films toward the menu
   (rebuilt that day, the previous binary kept as `~/omk/bin/omk-play.1003`),
   and the CARBON `OMKPlay` at the street start (`~/omk/cplay/omk.args`; the
   boot's args are `omk.args.boot`) - "carbon build is running correctly":
   25 bodies, 200 walkers, adventure mode, Sound Manager audio, 67 frames in
   35 s (~2 fps, the software renderer on the emulated G4). **Walked in
   BOTH builds** (SDL/GL1 and Carbon), the reader: "controls felt
   correct" - (a) is done on Tiger; **OS 9 by hand, 2026-10-04** (the
   street start from Startup Items), the reader: the MUSIC played, STOPPED
   when he started running and never came back, while the sound effects
   (footsteps) kept playing. NOT the mixer: the OS 9 disk's `omk:fr` had
   no `TRACKS` folder, so `music switch to 2` found no file and the game's
   music never played at all; what was heard was the street's own scene
   sounds (`anekbah.sfx`, one-shots of 6.6-8 s at the area load), which end;
   the footsteps are one-shots too. TRACKS (145 files, 187 MB) COPIED onto
   that disk the same day, byte-identical re-run, the reader: "good now" - the
   music plays on OS 9 through the Sound Manager ring at emulated speed.
   So (a) is done on Tiger AND OS 9; (b) ~~Sound Manager audio~~ DONE (`8b536ad`) -
   left: the ring is six 100 ms buffers refilled between frames, so a frame
   slower than 0.6 s underruns (OS 9 under emulation); a bigger ring costs
   the interface blips latency, so measure on a real G3 before choosing;
   (c) ~~the films~~ DONE without QuickTime (`32784eb`): the vendored
   pl_mpeg decodes them on PowerPC and the Sound Manager plays their 44100
   track - Tiger drops 3-9% of frames once the offscreen matches the
   window's depth; emulated OS 9 plays all three but drops ~74%, and the
   films stretch to minutes because they pace by an audio clock that
   underruns there; (d) speed - the software renderer is ~2 fps on the
   emulated G4. ~~the gl1 backend is CGL (Tiger only): AGL for OS 9~~ -
   **DONE 2026-10-04, the reader's ask ("integrate opengl in the carbon
   build, it would be easier to test on tiger")**: the Carbon `OMKPlay`
   draws the world through the fixed-function backend in an AGL context on
   its own window (`OMK_GL1_AGL`, `backends/gl1/gl1host.h`) - one binary
   for OS 9's OpenGL 1.2.1 and Tiger's. On Tiger: ATI Radeon 9700, OpenGL
   1.5, accelerated, the street drawn; ~5 fps, because the frame is read
   back (through a texture - see 5), composited on the CPU and drawn back
   with `glDrawPixels`; 17 fps with the world swapped straight, which is
   the next step: the interface drawn OVER the world in GL, as the GLES
   window's overlay does, instead of the world under the interface on the
   CPU. NOT run on OS 9 yet (its emulation has no 3D card: Apple's software
   renderer). `--software` in `omk.args` keeps the reference.
   Measure on REAL hardware first: emulated timing is not a G3's or a G4's.
   ~~**The gateway class**~~ - **DONE 2026-10-03** (`12d77ec`): the
   viewer's game code reaches the host only through `omk::Frontend` (the
   clocks, the window title, fullscreen, the display modes, text input and
   the error string joined the interface; `makeHostFrontend()` is the one
   function a frontend defines besides its class). 30 game files compile
   with no SDL header (`verify.py: engine: frontend gateway`, a poisoned
   `SDL.h`); the SDL side is `sdlfront.*`, `playgpu_*.cpp` and
   `playscene.cpp`. Proved by the 28-scene golden record and by building
   for the Mac, Tiger (run: the title through the gateway), the Vita and
   the software-only `INSTRUMENTS=0` variant. **What a Carbon frontend now
   implements**: `CarbonFrontend : omk::Frontend` and `makeHostFrontend`,
   plus a `playgpu_*.cpp` for its GPU window if it has one - `playgpu_gl1`
   is the model, since it presents on the CPU through the frontend. The
   play split had finished (S5/S6, 2026-10-02), so nothing was in flight.
3. **What is left on OS 9 after the frontend**: ~~the memory budget~~ -
   **MEASURED 2026-10-04** (`backends/classic/heapcount.*`: this build's own
   `operator new`/`delete` count every C++ block by `GetPtrSize`, exactly,
   and `FreeMem`/`MaxBlock` are sampled each frame; a flushed line every 30
   frames, the totals at exit). Retro68's `malloc` is `NewPtr`, so all of it
   comes out of the partition. On OS 9, the street start in Anekbah: peak
   **79886 KB** of live C++ blocks (during the load), **79009 KB** used by
   FreeMem at the low-water mark - about 82 MB with the C allocations and
   the blocks' overhead - and MaxBlock 5 MB under FreeMem (fragmentation).
   Kay'l's apartment (`games-resto.bin` slot 0): **49741 KB**. The
   conversation itself (402, ~600 frames in) was NOT reached: twice the run
   on Tiger ended early with exit 0 and no reason given (the frontend now
   prints each quit's cause), and OS 9 runs ~5 s a frame. **THE BUDGET IS
   64 MB, A GOAL** (the reader, 2026-10-04: "the original game works
   correctly with 32MB, we already ask for twice the requirement of the
   original") - so the street is **~18 MB OVER**, and that is the code's to
   fix, never the budget's: `classic-mac-port-1999.md` 3b's table is the
   list. The partition is 96 MB preferred (what today's code needs to reach
   the street), **64 MB minimum**. Against 3b's 136.8 MB on a 64-bit host,
   the 32-bit build holds ~80;
   `%zu` is handled (`classic_printf.h`), audio and the films work. `DataFs` and no threads
   are DONE (2 above); with `OMK_THREADS 0` the voice read-ahead already
   runs on its frame, so "ticked loading" is a speed question, not a gap.
4. **Speed**: the 1999 budget (`classic-mac-port-1999.md` §3b, steps 1-2).
   Tiger's emulated G4 is not a G4's timing.
5. ~~**A G3 build**~~ - **DONE 2026-10-04.** Our own objects were always
   generic (`PPC ALL`); the 119 objects of SDL were `ppc7400`, because its
   configure finds AltiVec and adds `-maltivec` to every unit, and the
   linker marks the binary with the highest subtype. Rebuilt in Tiger with
   `--disable-altivec` (`~/src/panther-sdl2-g3`, installed to
   `~/sdl2-tiger-g3`, copied to the tools volume) - which needed ONE patch:
   `SDL_cocoavideo.m` and `SDL_cocoamessagebox.m` include `altivec.h` under
   `__POWERPC__ && !__APPLE_ALTIVEC__` to undo its `bool`/`vector`/`pixel`
   macros, which errors without `-maltivec`; the guard now also asks for
   `__ALTIVEC__`. The binary is `ppc`; a scan of `otool -tv` finds 24
   vector instructions against 230 in the G4 build, all in libgcc's
   `save_world` / `eh_rest_world_r10`, which test `__cpu_has_altivec` and
   return before them on a CPU without it - and no `fsqrt`. Its 30-frame
   street dump is BYTE-IDENTICAL to the G4 build's on Tiger. Not run on a
   G3 (QEMU's `mac99` is a G4 here): the evidence is the subtype, the
   scan and the identical frame.
6. **The ppcosxkvm report**: test the fix seriously (the checklist in the
   draft), and narrow the second artefact (below) to a minimal GL repro.

---

## 5. Traps that cost time

* **ppcosxkvm is patched locally** (`scripts/ppcosxkvm-r300-txo-endian.patch`):
  32-bit textures with TXO_ENDIAN 2 drew byte-reversed (black as BLUE) -
  SDL 2.0.3's window texture is the only one. `ppcosx update` discards it;
  re-apply, `ninja qemu-system-ppc`, then `scripts/bundle-dylibs.py` on it.
  `OMK_R300_TEXLOG=1` logs each distinct texture setup.
* **Still open, suspected emulation**: a wall grazed by the camera flashes
  FLAT for a frame in Tiger; the M3 never shows it (60-position sweep, worst
  169 px). Not cured by the screen-edge clip.
* **`glReadPixels` FROM A WINDOW SURFACE RETURNS ZEROS on the emulated
  Radeon** (2026-10-04): the AGL build's world drew correctly - swapped
  straight, the street showed - while every read of the back buffer came
  back black, even a cleared red. The backend copies the back buffer into a
  texture (`glCopyTexSubImage2D`) and reads THAT (`glGetTexImage`). A frame
  at frame 1 is legitimately black (the area's fade-in): look at frame 30.
* **Retro68's `MakeImport` CRASHES on Tiger's `CFMSupport/OpenGLLib`** (its
  `cfrg` holds three fragments) - make the stub from OS 9's `OpenGLLibrary`,
  which exports the same names under the same fragment name.
* **`scp` DROPS A RESOURCE FORK**: an app copied that way fails with
  `cfragRsrcForkErr` (-2856), a library loses its fragment name. `ditto -c
  -k --sequesterRsrc` on the Mac, `ditto -x -k` in Tiger; onto the OS 9
  disk image, `cp -p` keeps it.
* **A CGL context on Tiger needs a DRAWABLE**, or nothing is drawn or read
  back, even into an FBO - the backend attaches a 16x16 pbuffer (a current
  Mac refuses pbuffers and goes on without).
* **Do not use the GPU's transform or fog on the emulated Radeon**: the
  first let near-plane triangles through as garbage, the second darkened
  the near scene. The backend does both on the CPU (as the original did).
* **`verify.py` gives every `omk-play` run `--res 800x600`** unless the
  check names a size - a dump of the wrong size first read as a bug.
* **`make` does not track flags**: after changing `PPCFLAGS`, delete the
  object directory, or the old objects are linked (it happened with
  `-ffp-contract=off`, which `ppc-darwin.mk` sets because GCC's fused
  multiply-add drifted a traffic run by 0.1 unit).
* **NO WEAK OBJECT WITH A CONSTRUCTOR** (an `inline` variable built at
  start-up, a static local in an inline function): Retro68's linker gives a
  weak `.bss` object NO STORAGE, so its constructor writes over its
  neighbour - `recursive_init_error` from a guard, or silent corruption
  (PORTING A10, `engine: classic build`). The symbol-table test: a
  C_WEAKEXT label whose BS csect has length 0.
* **`%zu` CRASHES on Retro68** (it consumes no argument; a later `%s` reads a
  size as a pointer) - `classic_printf.h` redirects the printf family; keep
  it force-included.
* **A run that "exits 0 and prints nothing"** is `std::terminate`: the
  classic entry now says so and faults on purpose, so Tiger's crash reporter
  writes the backtrace. Read it there.
* **Compare frames against a host built WITHOUT fused multiply-add**
  (`ppc-darwin.mk`, `-ffp-contract=off`); `build/omk-play` differs by float
  noise.
* **NO C++ STREAMS in anything the classic build links.** Retro68's linker
  lays libstdc++'s `num_get<char>::id` over `timepunct_cache_w`, so the
  locale's own set-up corrupts it and the first stream (even an
  `ostringstream`) crashes - but only in a LARGE program: a stream test
  passes alone, so a hello-world proves nothing. `src/` reads and writes
  files through `DataFs` (`readTextFile`, `writeWholeFile`); `engine:
  classic build` fails if stream symbols reappear. Bisecting the objects
  found it; a crash log's backtrace on Tiger
  (`~/Library/Logs/CrashReporter/<app>.crash.log`) names PEF symbols.
* **Retro68's `open()` takes an HFS path and opens read-WRITE**, and has no
  `stat`/`mkdir`/listing: everything goes through `DataFs` and
  `omk::hostPath()`. A '/' in a path is a character of a file name there.
* **Tiger's display sleeps after 10 minutes** and `screencapture` then writes
  nothing: `/tmp/omk-wake` in Tiger (a 3-line `UpdateSystemActivity` program)
  wakes it without root.
* **NO 8-BYTE ATOMIC ON 32-BIT POWERPC** (2026-10-08): GCC lowers
  `std::atomic<long long>` to `__atomic_*_8` calls into libatomic, which the
  ppc-darwin8 toolchain does not ship, so `omk-play` stopped LINKING once the
  profiler's memory counters landed (`2b47a8a`, 2026-10-04) and nobody
  rebuilt for Tiger until today. `profile.cpp` now picks the widest LOCK-FREE
  type (`long` here; they count live memory, bounded by the address space).
  Any new `std::atomic` of 64 bits in shared code breaks this build again.
* **Tiger has no `seq` or `pgrep`** (`jot`, `ps | grep`); a host loop that
  greps for `done` can be fooled by a script that wrote one early.
