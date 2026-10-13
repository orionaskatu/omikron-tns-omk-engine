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
