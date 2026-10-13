# Code readability audit (2026-10-12, at `0ffcc08`)

The reader asked for the whole code's readability and cleanness to be judged
before `tutorial/` is written, so that the tutorial teaches code worth
learning and a person can change it by hand. Read-only: nothing was changed.

How: measurements over the tree (below), then six reviewers, one per area,
each scoring the same eight criteria 1-5 with file:line examples, and a
spot-check of their sharper claims before recording them here. Scores are
judgements of sampled code (the big files were read in slices), the
measurements are counts.

## 1. Measured

| measure | value |
|---|---|
| engine C++ | ~150k lines: `src/` 67k, `backends/sdl` (the viewer) 30k, GPU/platform backends 19k; `engine/tools` 30k (217 probe files) |
| compiler warnings, `-Wall -Wextra -Wpedantic` over `src/` + the viewer | **8 distinct**, 0 errors - 74 of the 82 printed are one unused parameter in `src/platform/frontend.h:151-152`, repeated per translation unit |
| TODO / FIXME / XXX / HACK | **1** |
| comment : code lines | `src/` 0.42-0.80 (ui 0.80, script 0.77), the viewer 0.57, GPU backends 0.24-0.35, `engine/tools` 0.28 |
| functions over 300 / 150 lines (top-level scan, a floor) | 25 / 64 - nearly all in the viewer |
| the longest | `PlayState::worldStaged` **3572** (`playframe_world_staged.cpp:28`), `modesShoot` 1581, `worldCrowd` 1272, the world draw part 1085, an adventure part 969, `phaseWorld` 960; in `src/`: `UiWalk::confirm` ~950, `ScreenComposer::draw` ~1500, `Session::onCall` ~445 |
| god objects | `PlayState` ~880 member lines (`playstate.h`, 140 of them aliases into `opt`); `Session` (area.h+area.cpp 6.6k lines); `UiWalk` (~50 per-screen members); `PlayerController` (~98 members) |
| hidden state | **217** function-local `static`s in the viewer (71 of them `...Told` log change-detectors), 15 in `src/` |
| environment switches | **86** distinct `OMK_*` names, 137 reads, read four different ways |
| viewer `printf`s | 777 - a TEST INTERFACE: `verify.py` asserts on their exact wording |
| little-endian readers re-written locally | **29** files, against 11 using `formats/le.h`'s `loadLE` |
| raw `flags & 0x...` tests in `src/actor` | 105 |
| opcode enum | **none** - `case 118: case 119:` (area.cpp:2885) |
| `verify.py` boilerplate | `"make", "-s", "play"` x179, `SDL_VIDEODRIVER="dummy"` x194, `return ("build failed",)` x287 |

## 2. Scores (1 bad .. 5 excellent)

| area | naming | size | comments | state | duplication | errors | idiom | hand change |
|---|---|---|---|---|---|---|---|---|
| formats / platform / audio / input | 4 | 3 | 3 | 4 | **2** | 3 | 3 | 3 |
| script (Session, VM) | 3 | 2 | 3 | **1** | 2 | 3 | 3 | 2 |
| actor | 3 | 2 | 3 | 2 | 3 | 3 | 3 | 2 |
| o3de / ui | 4 | **1** | 3 | 2 | 3 | 4 | 3 | 2 |
| the viewer (`backends/sdl`) | 3 | **1** | 3 | **1** | 2 | 3 | 3 | 3 |
| GPU backends | 4 | 2 | 4 | 2 | 2 | 3 | 2 | 2 |
| `engine/tools` probes | 4 | 2 | 4 | 3 | 2 | 3 | 3 | 3 |
| `tools/verify.py` | 3 | 3 | 4 | 2 | **1** | 3 | 3 | 2 |

**The verdict.** Clean at the bottom and heavy at the top. The hygiene is
unusually good: almost no warnings, one TODO, defensive bounds everywhere, pure
format readers, a strict renderer boundary and an SDL-free game code, and
comments that carry EVIDENCE (an address, the original's order of arms) so a
reading can be checked rather than trusted. What hurts a person who wants to
change it by hand is concentrated in four places: giant functions, four god
objects, bare numbers where names belong, and comments that tell the story of
how a rule was reached on top of the rule.

## 3. The problems, ranked by how much they hurt a hand edit

1. **Giant functions.** `worldStaged` 3572 lines with 16+ levels of
   indentation, `modesShoot` 1581, `UiWalk::confirm` ~950 ordered callback
   arms whose ORDER matters (the sneak row arms at widgets.cpp ~2437-2570 fall
   through on `rowKind`), `ScreenComposer::draw` ~1500, `Session::onCall`
   ~445, `PlayerController::tick` 357, gl1 `submit` 338 / `presentWorld` 410,
   `readCtl` 275. The viewer's phases still end `do { ... } while (false);
   return done ? -1 : -2;` - main's old `break`, with unnamed return codes.
2. **God objects with cross-phase flags.** `PlayState`, `Session`, `UiWalk`,
   `PlayerController`. Nothing shows who writes or reads a member: `sideCullSet`
   is set in `worldStaged` and read in the world draw; `liftHandled` set and
   read 40 lines apart in different functions. `PlayerController` hands out
   mutable references to its internals (`eulerPitch()&`, player.h:499-510).
3. **Bare numbers.** No opcode enum; context statuses (`status == 7`), fight
   states and AI intents, shoot clip types, `.CTL` flag bits (105 raw tests),
   screen numbers (`screen_ == 5`), and some raw addresses used as sentinels
   (`e.textFn == 0x004AF5D0u`, `0x004780A0u` in screendraw.cpp). You cannot
   grep "where is fight.begin handled".
4. **Comments that narrate history.** ~25 in `src/actor`, ~71 in `src/ui`
   alone of the "until 2026-xx-xx this port..." / "a reader said..." kind, and
   blocks of 20-60 lines over 4-10 lines of code (player.cpp:890-912,
   staged.cpp:1772-1830). The `Session` declaration is ~1185 comment lines of
   ~1875; `UiWalk`'s 424 of ~700. The evidence (address, rule) should stay; the
   story belongs to git and `docs/`.
5. **Stale or misplaced comments - worse here than elsewhere, because the
   comments ARE the evidence.** Confirmed: `widgets.h:405` says one draw hook
   is ported (more than ten are); area.cpp:1077-1079 says `OMK_TUNNEL=1` gates a
   log that `const bool trLog = true;` prints always. Reported: the terminal
   family's and Gandhar's comment blocks above the Xachen arm (widgets.cpp
   ~1693-1755), screendraw.cpp:16-23 contradicting itself, area.cpp:2897 above
   the wrong case.
6. **Duplication.** The LE byte readers in 29 files with three different
   bounds rules (some return 0 silently, some check nothing); deg-to-rad in 8
   files; `vec3` helpers twice in o3de; the four GPU renderers each carry their
   own CPU posing, depth-tie bake, texture-scale rule and blend switch, which
   already differ in detail (which env var disables the tie; where the light is
   applied - GLES in its shader, the others on the CPU; a reviewer's "GLES
   applies no lights" was checked and is NOT a bug); `verify.py`'s
   build-and-run preamble pasted into every check; the probe preamble
   (`OpcodeTable::loadJson` in 62 tools) with no shared header.
7. **Hidden state and switches.** 217 function-local statics in the viewer,
   some keyed by actor id, that survive an area change or a load - a log line
   can then fail to print in a second session and a check depending on it go
   red for no visible reason. 86 `OMK_*` switches, some read inside hot loops.
8. **Checks pinned to prose.** Checks regex the viewer's English log lines;
   rewording a `printf` turns checks red with no compiler help and nothing at
   the `printf` says which check reads it.
9. **Smaller.** Terse or legacy member names shadowed by locals (`w`, `n`,
   `st`, `fr` after the `fr/` -> `gamedata/` rename); decompiler names kept in
   code (`v5` as the `.CTL` walk's cursor, `v9`/`v10`/`a3` in shoot.cpp,
   `m4CAD30` in fight.cpp); file names that mislead (`sliders.h` is the
   pedestrian + vehicle pool, `slider.*` the ride; `sneakRows` holds the
   terminal's text); platform code forked by `#if` inside one file
   (datafs.cpp 25 `#if` lines); 448 `static_cast<std::size_t>` in the viewer.

## 4. Strengths to keep (and to teach)

* **Evidence in the code.** Comments cite the address and the original's
  instructions, so a reader can verify a behaviour against the binary.
  `screensTerminals`' table and `walk.h`'s named thresholds with units are the
  models.
* **Clean seams.** The pure format layer (span in, struct out, never throws,
  32-bit-safe bounds); `renderer.h`'s decision-level boundary; `omk::Frontend`
  as the only door to the host; `WorldHooks` between the VM and the world;
  table-driven ACTOR_STATE edges (state.cpp).
* **Honesty about gaps.** An unmodelled path sets `approx_` and says so rather
  than guessing; requests are handed to the caller (`takeQuitRequest`), not
  acted on in place.
* **The spine.** `PlayState::run` and `step` list the setup sections and the
  six phases in order, each in a profiler zone; `verify.py` is one function a
  check with a docstring saying how it was shown to fail.

## 5. What to do - cheap (hours each)

* name the numbers: `enum class Op` generated from `tables/vm_opcodes.json`,
  `enum class CtxStatus`, `.CTL` and shoot flag constants, fight states and
  intents, screen ids, and the inline addresses as named constants;
* one byte reader: a checked `formats/bytes.h` over `loadLE` with one bounds
  rule, the 29 local copies deleted; one `kDegToRad`; one `vec3` header;
* fix the confirmed stale comments; move each arm's comment above its arm;
* move the "until 2026-..." history out of the code into `docs/` / git,
  keeping the rule and its evidence line;
* rename the shadowed members (`w` -> `uiWidgets`, `n` -> `frameNo`, `st`,
  `fr` -> `dataRoot`) and turn on `-Wshadow`; rename `v5`, `v9`/`v10`/`a3`;
* one `envFlag()` / `envNum()` reader for the `OMK_*` switches, read once;
* one `Told` helper owned by `PlayState`, cleared on an area change and a load,
  in place of the per-function statics;
* name the phase return codes;
* `verify.py`: a `play_run(...)` helper for the build-and-run preamble and a
  hold-string builder (`press("ENTER")`, `wait(40)`), migrated as checks are
  touched; `engine/tools/probe_common.h` for the probe preamble;
* the `frontend.h` unused-parameter warning (74 of the 82 printed).

## 6. What to do - expensive (days each)

* **break the giant functions along their own `---- BANNER ----` sections**,
  one method per banner, nothing reordered - so every `printf` runs in the same
  order with the same text and the stdout-asserting checks stay byte-identical;
  gate each step with that subsystem's `--only` checks plus a before/after diff
  of the full stdout of a few fixed `--frames N` runs. Innermost first (the
  gunman tick inside `worldStaged`), then `modesShoot`, `UiWalk::confirm` /
  `press` (per-screen handlers behind an address -> handler table),
  `ScreenComposer::draw`, `Session::onCall` (a per-opcode handler table with
  "visible" and "announce" as attributes of the same entry);
* **split the god objects into owned components**: `PlayState` into
  `SliderState`/`ShootState`/`CrowdState`/`ScreenState`/`CullState` taken by
  reference so a cross-phase read shows in a signature; `Session` into
  transition, camera, fade/music, context table, actors, props; `UiWalk`'s
  per-screen state into small structs; `PlayerController` into follow camera,
  water, head look, root motion;
* **one shared CPU-pose + light + tie + blend implementation** used by the four
  GPU renderers, with a check that they agree;
* **machine-readable event lines** (`@evt key=value`) beside the prose, for
  checks to read instead of English;
* per-platform files for `datafs` / `threads` / `profile` instead of `#if`
  forks; the `.CTL` / `.SCX` walkers one function per section.

## 6b. The refactor guard, and a nondeterminism it found (2026-10-12)

`tools/refactor_guard.py` records the whole stdout of thirteen fixed runs -
the boot, the menu, the street, both terminals, a melee fight, shoot mode, a
slider boarding, Astaroth, the sneak's memos, Xachen, the Gandhar door, a set
render - with each run's last-frame hash, and diffs two recordings. It is
the proof for every behaviour-preserving step below: `record base` before,
`record now` after, `diff base now` must print `0 of 13 runs differ`. Shown to
work: the Gandhar door's 1500 ms close set to 1600 moved its close from frame
208 to 211 and the guard named it. Two recordings of one tree agree 13 of 13
once the timing lines (milliseconds, the music level) are dropped.

**Found on the way, not yet explained**: a run that ENDS INSIDE A CONVERSATION
(Xachen to frame 600, the Dakobah/Xendar conversation from frame 468) logs the
same lines on every run but its LAST FRAME differs between two runs of the same
tree - so something in a conversation's picture is not deterministic under
`--frames` (the line stream reads no thread; the interface clock is
`n * 1000 / 30` there). The guard's Xachen run stops at 460 for now. Worth
finding: a check that hashed a conversation frame would flicker.

## 6c. Progress

* step 0 - `tools/refactor_guard.py`, `af7f15f`;
* step 1 - one byte reader, `formats/bytes.h`, `52e5cb1` (and the Vita
  build's missing weather file and the licence census, red since 2026-10-11);
* step 2a - the opcodes by name, `engine/src/script/opcodes.h` generated from
  the table, `verify.py: opcode names` (shown to fail on a stale header and on
  a bare `case 136:`). Follow-up: the table's 146/147 `ambience.on/off` are
  the radar's switch - rename the row with the docs and the name counts.
* step 2b - the context status word, `script/ctxstatus.h`, `4bb555a`;
* step 2c - the `.CTL` entry flags (`ctl_entry::`, in `formats/ctl.h`) and
  the channel's own (`channel_flag::`), `eca6446`. **A trap it found, for the
  tutorial's lesson on traps**: replacing `flags_ & 0x12` by a NAMED COMBINED
  MASK wrote `flags_ & Blending | ClipRanOut`, which `&`-before-`|` reads as
  `(flags_ & Blending) | ClipRanOut` - always true - and one blended path went
  dead. The guard's thirteen viewer runs were IDENTICAL; `engine: actor
  states`' 395352-tick sweep moved by 8. So a guard is only as wide as what it
  runs: it now records engine PROBES too (`run_actor_states` first), and a
  name that stands for several bits is always written in parentheses.
* steps 2d-2h - the shoot record's, the mover's and the fighter's flags
  (named only where something established them), the 37 screens
  (`ui/screens.h`), the interface's inline addresses, the decompiler names
  (`v5`, `a3`/`v9`/`v10`); the fight's `m4CAD..` tables keep their address
  names by rule. Found on the way: `engine: training partner`, red since row
  24's 4500 ms simulator close (a 700-frame run left two paired seconds).
* step 3a - the confirmed stale and misplaced comments, `cf2b9a5`, and
  `tools/comments_only.py`, which proves a change touched comments only.
* step 3b - the HISTORY NARRATIVE out of the code: 217 files, ~900 passages
  rewritten into the present-tense rule with its evidence, by six agents under
  one rulebook. Measured over the comments before and after: addresses
  3632 -> 3632, backtick quotes 16775 -> 16776, check names 148 -> 148, dates
  602 -> 14 (the rest are provenance kept by rule), "a/the reader" 256 -> 24
  (the rest mean a file reader), 386 comment lines shorter; `comments_only`
  0 code changes, the guard 0 of 14 differ. Four stale CLAIMS corrected on the
  way: `walk.h` (the capsule sweep is in, the drop refusal retired),
  `player.cpp` (the swim probe is the head, not the pelvis), `renderer.h` (the
  mirror's confinement IS traced - draw order and depth; only the CPU
  fallback's mask is a reconstruction), `interp.cpp` (op 62's AI level is
  carried).
* step 4 - the shadowed members renamed (`fr` -> `dataRoot`, `w` ->
  `uiWidgets`, `n` -> `frameNo`, `st` -> `devices`) and `-Wshadow` ON in the
  main Makefile: the 46 sites it then named (engine, viewers, tools) renamed
  inside their own scope only, none of them a real bug. Proof: every object
  of `make`, `play`, `play-gles` and `vulkan` built at HEAD and after, paths
  normalised - 387 of 391 BYTE-IDENTICAL; two differ only in a renamed
  static's symbol (text identical) and two (`playharness.o`) in the one
  intended fix - `--scx-play`'s line printed `frames`, the `--frames` limit,
  as a `%ld` where it meant the frame (`frameNo`). The 3DS and Vita builds
  rebuilt clean; the GL1 glue syntax-checked. **A trap, for the tutorial**:
  the first pass of this rename (interrupted by a crash) missed
  `playgpu_gl1.cpp` and `playgpu_citro3d.cpp`, PlayState methods that only
  the PowerPC and 3DS makefiles compile - so a rename that the Mac build
  calls complete is not, and a rename of a `PlayState` member must build or
  at least grep every platform's glue. `-Wshadow` is not on in the Vita,
  3DS, classic or PowerPC build files yet.
* step 5 - ONE READER for the `OMK_*` switches, `src/platform/env.{h,cpp}`:
  `envFlag` (set at all), `envOn` (starts with '1'), `envInt`, `envNum`,
  `envStr`, over a snapshot taken at the first read, so no `getenv` runs in
  a frame loop any more (`area.cpp` called it uncached on every camera move,
  transition and zone prune; `envSet`'s 32-entry cache was too small for the
  names it served) and a worker thread may read one. All 86 switches sit in
  ONE TABLE with a line each on what they do and the check that sets them;
  a `static_assert` keeps it in `strcmp` order for the binary search, and
  `verify.py: env switches` (fast list, a source scan) holds table and code
  to each other both ways and refuses a raw `getenv("OMK_...")` - shown to
  fail on a raw read put back, a row deleted, a row nothing reads. 143 reads
  converted, `envSet` gone. Proof: the guard 0 of 14 differ (switches off),
  and the seven checks that SET a switch of each shape green (`fight park`,
  `address camera`, `astaroth floor`, `weather`, `program placement`,
  `mirror park`, `pool past 64`); the 3DS and Vita builds clean. None of the
  `OMK_*` names `verify.py` sets is dead: the seven not in the table are its
  own settings, build defines and paths.
* step 6 - THE STATICS THAT HOLD STATE, read one by one (the reader's
  choice, 2026-10-13, over §5's `Told` helper). Measured first: the viewer
  has 156 mutable function-local statics - 78 log dedups ("told") in at least
  six shapes (once, on change from a chosen first value, on a threshold, per
  actor once, per actor on change, rate-limited with `>` or `>=`), so one
  helper would either bend their semantics or carry every variant, and its
  payoff is a log line printed again after a load; they stay. Of the other
  78, ~50 are scratch buffers, instrument counters and harness parsing, and
  ~25 hold state - read, and FOUR were wrong, each surviving a new body, a
  placement or another screen:
  - `lastGood` (the hidden-set undo, `Walk_ProbeGround`'s state-1 arm): the
    engine undoes THIS FRAME'S MOVE; the port puts him back where his last
    step was accepted, which after a placement or a new body is the place
    before it. Now a `PlayState` member, forgotten on both.
  - the root's vertical sum (`rootAccum`/`rootLast`/`rootState`): per BODY
    in the engine (it is the node's offset), carried to a new controller
    here with a stale base. Members, reset where the controller is built.
  - the player's head-mesh index, cached by the mesh vector's ADDRESS and
    size twice over: a rebuilt model landing on the freed block with the
    same count kept the old body's index. `playerHeadMesh`, set where the
    meshes are read.
  - the shop's Analyser asked `UiModels::examine` only when the row
    changed; the slot is shared with the sneak and the multiplan, which ask
    every frame, so a preview loaded there since showed in the shop. Asked
    every frame now (the slot keeps its stem: free); only the log is gated.
  The rest are sound as they are: input edges and the flush re-arm
  themselves, the media column and window title are keyed by their inputs,
  `standApplied`/`characterProgPlayed` are once-per-run harnesses,
  `characterBuiltFor` loads synchronously. Proof: the guard 0 of 14 differ
  (no run meets a stale case), `shop open`, `head look`, `player vertical`,
  `player jump`, `player landing`, `cupboard take` green; 3DS and Vita
  build. **`engine: security rail` is RED at `cb68eb6` already**
  (`(1, '346.5')`: one fall on level -2's running route, where none is
  expected) - the same values before and after this step; recorded, not
  bisected.
* step 7 - the phase return codes by name: `phase::Continue` (-1) and
  `phase::EndLoop` (-2) in `playstate.h`, any value >= 0 the program's exit
  code; every setup section, phase and part (`inputPump`, `modesShoot`,
  `modesQuitLoad`) and both loops (`run`, `step`) use them. Guard 0 of 14.
* step 8 - ZERO WARNINGS in `make`, `play`, `play-gles` and `vulkan` (was
  ~760 lines a full build, 8 distinct in the engine): `frontend.h`'s default
  `playSound` names its ignored `loop`/`gain` in comments; `vkrender.cpp`
  silences `-Wmissing-field-initializers` for itself, with the reason (the
  `{VK_STRUCTURE_TYPE_...}` idiom zeroes the rest on purpose);
  `Session::reloadScene` LOSES its `scene` parameter, unused because the
  `.SCX` is the AREA's; `playscene.cpp`'s four enhancement flags are
  `[[maybe_unused]]` (the Vulkan build alone takes them). In the probes: the
  unused locals gone - three kept their CALLS, which write through reference
  arguments (`bandOf`'s `moved`, `shootGenericStep`'s `y`); `zone_probe` and
  `parking_probe` name every `RunStatus` instead of printing `?` for three;
  `sprite_dump` prints its `size_t`s with `%zu`. And the one warning the
  noise had hidden: `scenerunner.cpp`'s POSITIONAL `Started{...}` (CLAUDE.md
  1's "a positional initialiser shifts" trap) is DESIGNATED now. Checks that
  run those probes green (`parking ops`, `zone pump`, `dialogue play`, `line
  stream`, `shoot generic`, `shoot range`, `shoot fire`, `cull`, `sfx set
  pieces`); guard 0 of 14; 3DS and Vita build.
* step 9 - the shared PREAMBLES, migrated as touched (§5's rule):
  `verify.py`'s `_play_built(target)` / `_play_out(play, *flags, save=,
  env=)` (the build, the headless run against the data and the tables, the
  log as latin-1 - 196 checks repeat it by hand) with `_PLAY_UNBUILT`, and
  the `--hold` builder `hold(wait(40), press("TAB", frames=3), press("UP",
  "SPACE", frames=12))` over the parser's own grammar (`+` joins keys); and
  `engine/tools/probe_common.h`'s `omk::probe::open(argc, argv, min,
  usage)` for the `<gamedata> <tables>` preamble 64 probes repeat (the
  opcode table, `IAM`, `newGame()`). First users: `engine: fight park`,
  `engine: player jump`'s hold string (the builder reproduces the literal
  byte for byte), `checkpoint_probe` (its output identical before and
  after, its usage exit 2 kept). Those checks and `inventory checkpoint`
  green; `licence headers` 614.
* step 10a - ONE `kDegToRad`, ONE `vec3`: `src/o3de/vecmath.h` holds
  `kPi`, `kDegToRad` (= `dbl_4BCF00`'s pi/180, 0x1.1df46a2529d39p-6),
  `kRadToDeg` and their float forms, and `sub3`/`dot3`/`cross3`/`len3`/
  `norm3`. Measured first: the factors were spelled ten ways, and every FLOAT
  spelling rounds to the same float, while the double `0.0174532925199433`
  (17 sites), `3.14159265358979` (9) and `3.1415927` (3) are NOT the engine's
  number - so this step moves only the BIT-IDENTICAL sites (67 literals and
  ten local aliases - `fight.cpp`'s `kDegToRad`, `pose.cpp`'s `kDeg`, the
  address-named `kRad`/`kD2R`...), and keeps `x * kPiF / 180.0f` as that
  shape (it rounds differently from `x * kDegToRadF`). The helpers' copies in
  `render.cpp`, `raster.cpp`, `xrspace.cpp`, the VR camera and
  `pedestrians.cpp` (whose `len3` was an out-of-line `omk::len3` the
  vehicles shared through `sliders.h`) are gone; `particles.cpp`'s `norm`
  stays its own, its threshold differs. Proof: every object built at HEAD
  and after - 385 of 392 byte-identical, three with identical machine code
  (data and symbols moved), and `pedestrians.o`/`vehicles.o` re-laid by
  `len3` now inlining (the same expression); the guard 0 of 14 and the
  crowd, traffic and slider checks (`pedestrians`, `city crowd`, `road
  traffic`, `traffic frame`, `crowd push`, `slider fly`) green. 3DS and Vita
  build; `licence headers` 615.
* step 10b - THE SHORT CONSTANTS WERE NEVER THE ENGINE'S. Searched in the
  binary's bytes: pi/180 and 180/pi as exact doubles six times each, the
  port's `0.0174532925199433` never, no double pi at all, and pi, 2 pi, pi/2
  and `slider.cpp`'s 37 and 170 degrees as FLOATS. The short spellings are
  Hex-Rays' renderings (15 digits for a double - `shootfire.cpp`'s comment
  quotes `v54 = -sub_47D4C0() * 0.0174532925199433` - 8 for a float), copied
  into the port as decimals. So: the 17 short doubles and the port's own
  `deg * 3.14159265358979 / 180.0` (4, `shoot.cpp`) are `kDegToRad` now, the
  other short pi spellings `kPi`, the last local aliases (`player.cpp`'s `k`,
  `slider.cpp`'s `kDeg`) gone, and `slider.cpp`'s grip and slide angles are
  written as the floats they are (`3.1415927f`...: an x87 `fcomp` of a dword
  compares the float's own value). A FIDELITY change of up to an ulp - and it
  moved nothing measurable: the guard 0 of 14 (last-frame hashes included),
  and 15 checks over the touched code green (`shoot range`, `shoot
  generic`, `shoot fire`, `shoot gunfire`, `shoot move`, `astaroth tick`,
  `slider fly`, `slider journey`, `slider bank`, `scene facing`, `props`,
  `set pieces`, `shoot radar`, `frame rate`, `stairs`). **A lesson for the
  tutorial**: a constant copied out of a decompilation is the decompiler's
  PRINTING of it; when the value matters, read the bytes.
* step 11 - the fight's last bare numbers (§5's "fight states and
  intents", which step 2f's flags had not covered): `fight_role::` for a
  fighter's role (the `.CTL` entry's `+12` low byte - Standing, High/
  LowGuard, Down, ThrowTry/Throwing/Thrown, Far/NearSwitch, GettingUp),
  `fight_intent::` (Guard, CloseIn, AfterPair) and `fight_cam::` for
  `Fight_TickCamera`'s six arms - named only where the code establishes
  them; roles 2, 7, 9, 15, 16, 32 and intents 2, 8, 10, 105, 108 stay
  numbers and the headers say so, and `Fight_ResolveHit`'s `inStateN`
  booleans keep the decompiled function's own shape. `fight.o`
  byte-identical before and after.
* step 12a - THE GIANT FUNCTIONS, innermost first (§6): `worldStaged`'s
  shoot-mode block - the gunman's brain, move, fire and clip, Astaroth's
  and Gandhar's ticks, lines 505-2490 - is `PlayState::worldStagedShoot`
  in `playframe_world_staged_shoot.cpp`, moved verbatim (dedented, nothing
  reordered). It needed only the session alias from the enclosing loop and
  hands back the three things the pose reads (`shootTracks`, `shootFrame`,
  `astGrid`); every `return`/`break`/`continue` in it was inner, which the
  compiler confirms (a stray one does not compile in a void method), and
  step 4's `-Wshadow` at zero is what proves no former local now binds to
  a member of the same name. `worldStaged` 3572 -> 1594 lines. Guard 0 of
  14; 3DS and Vita build (the Vita's CMake lists the viewer's files by
  name - added).
* step 12b - `worldStagedShoot` along its own banners, by a MECHANICAL
  extraction: a range of whole statements moves verbatim into a new method;
  the compiler names the locals it needs ("undeclared identifier", iterated,
  since clang's recovery hides some behind others) and their exact types (a
  `decltype(x)::probe` line, and `std::is_const` for the constness clang's
  type printing drops); they arrive by reference, so writes reach the
  caller; a lambda-typed local, a top-level `break`/`continue`/`return`, or
  a form that does not compile refuses the move. libc++'s internal spellings
  (`std::__map_iterator<...>`) are mapped to the standard ones, since the
  Vita, 3DS and PowerPC compile with libstdc++. Two lambdas used across
  blocks became methods first (`shootApplyAction`, and `astPlace` a file
  function). Thirteen methods out: the clip tick, the gunman's first tick,
  `Shoot_ActorEnter`'s properties, THE BRAIN (Astaroth's, Gandhar's or the
  generic one - the generic is the `else` of that chain), the move decision,
  the patrol, the edge walk, the state's clip, the arrival, the clip
  advance, the strike, the fire gate and the turn clip. Guard 0 of 14; all
  32 shoot/Astaroth/Gandhar engine checks green; 3DS and Vita build.
* step 12c - `modesShoot` (1574 lines): four blocks out by the same tool -
  `modesShootEnterLeave`, `modesShootResume`, `modesDialogEnterLeave`,
  `modesScreenWalk` (839 lines, still to split). `modesScreenOpen` was
  REFUSED: it returns the program's exit code (`return 1` when a script's
  screen has no panel), so it needs a phase-code return before it can move.
  Guard 0 of 14. **Where this stands (2026-10-13)**: 34 functions over 300
  lines by a brace-matching count (the audit's 25 used a narrower scan) -
  next `worldStaged`, `ScreenComposer::draw`, `worldCrowd`,
  `worldDrawLists`, `modesScreenWalk`; the src/ classes need the tool
  generalised past `PlayState`. The rest of §6 (god objects, the shared
  renderer core, event lines, per-platform files) is not started.
* step 12d - the extraction tool COMMITTED as `tools/extract_method.py`
  (`extract`, `batch`, `candidates`, `sizes`), generalised past
  `PlayState` to any class whose header defines it, a `const` method's
  pieces const too. Two fixes the first runs forced: an INITIALISER's braces
  (`for (... : {&a, &b})`) are not a block, and a local that shares a member
  FUNCTION's name - `run`, `step`, `fn` in the viewer - is not caught by
  `-Wshadow`, so one declared before the range and used in it is passed like
  any local (a moved `name(...)` would otherwise call the method; checked
  over the earlier cuts: none of those names is CALLED, and any other use
  fails to compile against a function). `worldStaged`: four more blocks out
  - the scene-clip change, the line pose (16 parameters: the coupling the
  audit asked to see), the head look, the root motion - 1565 -> ~1100 lines.
  Guard 0 of 14; `scene facing`, `head look`, `beat handover`, `program
  placement`, `line facing`, `dialogue play`, `walk-in scene`, `training
  partner` green.
* step 12e - `worldCrowd` (1236 lines) into the pedestrians-and-traffic
  half and the player's (`worldCrowdPedestrians`, `worldCrowdPlayer`, no
  parameters either), with the walkers, the vehicles and the player's root
  drop their own; `worldDrawLists` (1082) loses the particles, the sky, the
  set runs, the mapped shadows' light and the classic shadows (`keyOf` a
  file function first - it captured nothing). The tool now drops an alias a
  cut left unused (`tidy`), and qualifies a type clang printed bare by its
  own "did you mean" (`Pedestrian` -> `omk::Pedestrian`); its member-name
  guard reads only real declarations. Guard 0 of 14; the crowd, traffic,
  shadow, particle and clip checks green.
* step 12f - `worldCamera` (856) along its `if ... else if` chain: the
  editing, slider, ride, taken, follow and script cameras' arms each a
  method (their interiors, none needing a parameter); `phaseWorld` (949):
  the player's become, every shown actor, the player's program, the sets,
  the dialogue camera; `screensSneakRows` (867): the verbs, the map, the
  identity, the examine and the inventory rows (`uiSound` a method first,
  `uiSoundPcm`); `adventureScreenInput` (964): the opponent's motion, the
  fight's report and end, the shoot mover, the water entry and the fall,
  slide and landing. The tool's batch now tidies aliases ONCE, at the end -
  a tidy deletes lines above the cuts still to come, which mis-placed one
  cut by a line (`phaseWorldPlayerBecome` took the `if`'s interior: still
  whole statements, verbatim, compile-checked). Guard 0 of 14; 57 camera,
  sneak, fight, water and landing checks green but one - **`engine: arrival
  camera`, red since step 2a**: its structural row matched `case 73:`, which
  2a renamed `case op::ActorGotoAddress:`; it matches either now and is
  shown to fail with the dispatch removed.
* step 12g - `ScreenComposer::draw` (1461), the tool's first class past
  `PlayState`: the load panel's selection, the examine text, the map pins,
  the identity and characteristics pages, the high scores, the row mark and
  fill (with the `itemFill` lambda only it used), the item sprite - and the
  ITEM LOOP'S BODY, `drawItem`, then its banner sections (the previews, the
  cursor, the text). The draw-hook addresses defined inside the loop are at
  file scope with their siblings now (two had arrived as parameters).
  Sections of 5-12 lines that became four-parameter wrappers were inlined
  back - a split must read better than what it replaced. **A hole in the
  tool, found and closed**: a nested bare `return;` left the ENCLOSING
  method; moved into a void method it leaves only that, and compiles - the
  tool now refuses one (an audit of all 61 methods it had made: none had
  one; a stray `break`/`continue` cannot slip, it does not compile in a
  method of its own). `--loop-body` moves a loop's whole body with its
  `continue`s as `return`s, ownership read from the braces (`for`/`while`/
  `do` own a `continue`, a `switch` a `break`, a lambda a `return`; a
  brace-less inner loop refuses). And `gles_probe.cpp` did not compile
  since step 10a (`kPiF` unqualified outside `namespace omk`), invisible to
  every build but its own target - `engine: texture filter` was red for it;
  `make build/gles_probe build/vita_bench` belongs beside `make play` from
  here on. Guard 0 of 14; 46 screen checks green.
* step 12h - `UiWalk::confirm` (947): fifteen of its callback arms are
  methods (`confirmXachenCartridge`, `confirmGandharCell`,
  `confirmTerminalCell`, the shop's row and sale, Multiplan's row and
  destroy, the hint purchase, the save, the load, the sneak's combine, its
  slider row, memo reader and examine), still tried in the SAME ORDER - the
  order the audit warned about is now a list of calls one can read down. By
  the tool's new `--returns`: an arm that returns from `confirm` becomes a
  `bool` method whose every non-lambda `return X;` is `{ retVal = X; return
  true; }`, called as `if (bool retVal{}; confirmX(it, retVal)) return
  retVal;`. The member-name guard drops a name it forced in when no local of
  it is visible at the range (another arm's, out of scope). Guard 0 of 14;
  83 screen, UI and walk checks green.
* step 12i - five more viewer methods: `worldStaged` (the program found by
  name is left - it needs the `nodeOwner` lambda - but the program's drive,
  the dead gunman's root and the turned pose are out), `modesScreenWalk`
  (the hosted options page and its input, the slider's travel, a save, the
  closing), `screensHuds` (the fight and shoot HUDs, the radar, the
  interface layer, the media inset, the subtitle), `inputMotion` (every
  scene's motions collected, `OMK_MESH_AT`, the soups patched in place, the
  split verify - one slot's motions need `Patch`, a struct local to the
  function, so they stay) and `adventureShot` (the shot's request, and the
  special-move loop's body `adventureSpecialMove` with its `MDACTION` arm
  `adventureActionMove` and the take). Two cuts were INLINED BACK - the
  take's step and refusal wanted 23 and 16 parameters, one state machine of
  locals that wants a struct, not a cut - and the tool now refuses past 12
  (`--max-params`) and can undo a cut (`inline`). The brace-less-loop rule
  is precise now: only a `continue`/`break` inside such a loop's own
  statement is that loop's. Guard 0 of 14; 56 checks over the five files
  green.
* step 12j - `worldStaged`'s loop body is `worldStagedBody` (`drivenBy` a
  file function, `nodeOwner` a method first - both lambdas over the
  loop), and from it the program found by name, the skin gate (by
  `--returns`: its `continue` had become a `return`), the floor under a
  body with no pelvis anchor, and the body's instrument lines. `worldStaged`
  3572 -> ~140 lines; what is left in `worldStagedBody` (686) is straight
  code sharing a dozen locals - the set's lights section was refused at 13
  parameters - which wants restructuring, not cutting. Guard 0 of 14; 19
  staging checks green.
* step 12k - the shoot brain's three arms by STRUCTURE (Astaroth's tick,
  Gandhar's, the generic brain - a first attempt that searched for the
  `else` text matched one inside Astaroth's own arm and was inlined back),
  and `setupSession`'s save load (`--returns`: it can `return 1`) and
  placement. Runs of blank lines the inlines left collapsed
  (`comments_only.py`: 0 code changes). Guard 0 of 14; 42 shoot, boss and
  session checks green.
* **STEP 12 MEASURED** (brace-matched function bodies over `engine/src` and
  `engine/backends`), before it and after:

  | | largest | >1000 | >600 | >400 | >300 | lines in functions >300 |
  |---|---|---|---|---|---|---|
  | before (`21cf6d6`) | 3550 | 5 | 16 | 19 | 32 | 23772 |
  | after | 829 | 0 | 3 | 15 | 36 | 15338 |

  The count over 300 ROSE while everything else fell: each giant left
  pieces in the 300s. **What is left over 600, and why**:
  `Interpreter::resume` (829) - a fetch-dispatch loop whose arms `continue`
  the loop or `return` from the interpreter; splitting it wants a per-opcode
  handler returning next / return / fall through, which is the table the
  audit filed as its own change. `worldStagedBody` (686) - straight code
  sharing a dozen locals (the lights section refused at 13 parameters);
  it wants a per-body state struct. `PlayOptions::parse` (635) - one flat
  `else if (a == "--flag")` chain, as readable as a table of the same rows.
  `Session::onCall` (428) stays a switch: its largest arm is 36 lines.
* **the CONSOLES were broken by step 12 and are fixed** - the 3DS from
  12g, the Vita from 12f-ish: clang prints a local's CANONICAL type, so
  `std::uint32_t eff0[3]` became the parameter `unsigned int (&eff0)[3]`,
  and on the consoles' newlib `uint32_t` is `unsigned long` (`size_t`
  `unsigned int`) - no binding. Every one (`eff0`, `standFl`, `uiBits`,
  `movedSoup`/`movedSteep`, six `std::size_t` counters) is back to the
  source's spelling, both console builds compile, and the TOOL now takes a
  parameter's type from the local's DECLARATION (`declared_type`), keeping
  clang's only for an `auto` local - and refusing one of a platform-sized
  integer passed by reference. Building the consoles belongs to every step
  that touches shared code; the 3DS's `make -k` gave the whole list.
* step 13a - PER-PLATFORM FILES for the data layer (§6): `datafs.cpp` 559
  -> 280 lines and 25 `#if` lines -> 0, over `platform/hostfs.h` - four
  questions (`isDirectory`, `pathExists`, `listNames`, `comparablePath`) and
  a read-only open / seek-and-read / close - plus the public `hostPath`,
  `makeDirectories` and `fileSize`, implemented in `datafs_posix.cpp`
  (`std::filesystem`, C stdio: the desktops and the 3DS),
  `datafs_vita.cpp` (`sceIo*`) and `datafs_classic.cpp` (the File
  Manager), each moved verbatim and compiled only on its own host. The
  resolver and the write guard are one code everywhere; the whole-file and
  range reads are one loop over `hostfs::readAt` (a failed seek still
  returns empty without a message). The classic file is not built here (no
  Retro68 on this machine): `engine: classic build` is what will. Guard 0 of
  14; `DataFs`, `hostile files`, `hostile save`, `archive chunks`,
  `settings resolve` green; 3DS and Vita build; `licence headers` 621.
* step 13b - `threads.cpp`'s three pools in their own files
  (`threads_std.cpp`, `threads_vita.cpp`, `threads_none.cpp`), and
  `FileFetch`/`BackgroundJob` one code over `platform/hostthread.h`'s
  `spawnDetached` and `sleepMs`: `threads.cpp` 465 -> 100 lines, 15 `#if`
  lines -> 0. **A fault the split made and the guard caught**: each file
  tested `OMK_THREADS` BEFORE including `threads.h`, which defines it, so
  on the desktop (and the Vita) the no-threads pool compiled and linked in
  place of the real one - everything built, every frame was right, the
  pool had NO workers. The guard's `bodies: posed over 8 runners` line went
  to 1 in 11 of its 14 runs; `engine: threaded bodies` and `thread_probe`'s
  mismatch count stayed green, one runner being correct too. Fixed (the
  header first), and `thread pool` (fast list) now asserts the workers -
  shown to fail with the include moved back below the `#if`. The header's
  claim that the Vita half had never been compiled or run is gone; it is
  both. Guard 0 of 14; 3DS and Vita build; `licence headers` 625.
* step 13c - `profile.cpp`'s forks: most were `#if OMK_THREADS` around a
  lock or a thread id, and `platform/sync.h` now holds that fork ONCE -
  `Mutex`, `ScopedLock`, `ThreadId`, `thisThread()`, empty stand-ins
  without threads (the 3DS compiles that half). 19 `#if` lines -> 11; the
  rest are not threads-or-not but the CPU (the counters' atomic width,
  64-bit where lock-free, 32 on PowerPC) and per-platform diagnostics (the
  Mac's frame-chain walk, the 3DS's return-address words, the Vita's
  shared `thread_local`) - one platform's tool each, left where they are
  read. Guard 0 of 14; the profiler checks and `engine: release build`
  green; 3DS and Vita build; `licence headers` 626.
* step 13d - the WALKERS one function per section where a section stands
  alone: `readCtl` 276 -> 196 (the fight AI's profiles and its input words,
  the effect records, the clips - a fifth, the state graph, refused at 15
  parameters), `readScxStream` (the block re-walked for the stream's order
  and registry rows; the payload walk refused at 15). The tool learned FREE
  functions (a `static` function placed above the one it came from, no
  header) and a batch now finds each job by its start line's TEXT - a cut
  above the jobs still to come shifts them - and the declaration reader
  masks the comment above a statement. Guard 0 of 14; the 13 `.CTL` and 21
  `.SCX` checks green; 3DS and Vita build.
* step 14a - MACHINE-READABLE EVENT LINES (§6): `platform/events.h`'s
  `omk::event(name, {{key, value}...})` prints `@evt name k=v` beside the
  prose line it mirrors, only when `OMK_EVENTS` is set (in the switch
  table), so every existing check and the guard see what they always saw;
  `verify.py`'s `_events(text)` reads them as records. The first events:
  `fight.begin` (how = attached / refused / bank-missing), `fight.begins`,
  `fight.park` (Session's own: the frontend could not start it) and
  `music.switch`; `engine: fight park` reads THEM now, not its five English
  lines. `engine: events` (new) asserts the contract - no `@evt` without
  the switch, the keys a check relies on with it - shown to fail on a
  renamed key and on the gate removed; `fight park` fails on the key too.
  CLAUDE.md §4 has the rule: a new check adds an event beside the line it
  needs. (A first cut put an event under a brace-less `if`, which would
  have fired always - caught before building on it; braced.) Guard 0 of
  14; 3DS and Vita build; `licence headers` 627.
* step 14b - OWNED COMPONENTS in `PlayState` (§6), by
  `tools/group_members.py`: a run of fields under one banner, read and
  written as one thing, becomes a nested struct, its shared prefix dropped
  and every reference rewritten (code, and backticked comment mentions) in
  the files that implement the class; a clash, a line mixing other names,
  or a failed build refuses and restores. Four so far: `sliderCam` (the
  slider's `Camera_Request` state, 15 fields - `sliderCamFromEye` is
  `sliderCam.fromEye`), `gun` (the gunmen per actor, 14), `crowd` (the
  pedestrians' jobs, bodies, LOD caches, clip library and counts, 15) and
  `traffic` (the vehicles, 5). A field's place in the struct is what
  states what it belongs to; a read across components now shows in its
  spelling (`crowd.drawn`, `traffic.drawn`). Guard 0 of 14; 60 slider,
  shoot, boss, crowd and traffic checks green; 3DS and Vita build, the GL1
  glue syntax-checked; `licence headers` 628.

## 7. For the tutorial

The tutorial can be written on the code as it is - the seams it teaches
(formats, the VM's hooks, the renderer boundary, the frontend, the checks)
are clean - but four cheap items would each remove a whole page of "beware"
from a lesson: the opcode enum (lesson 4), the single byte reader (lesson 2),
the `play_run` helper (lesson 9) and the confirmed stale comments (lesson 6
teaches from `widgets.*`). The giant-function breakup is the change that would
most help a hand edit, and is worth doing BEFORE lessons 5-7 point people into
`worldStaged` and `UiWalk::confirm` - otherwise those lessons teach a map of
code that is about to move.
