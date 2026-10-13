# Lesson 4 — The script VM

**Goal:** read a world script, follow one instruction through the port - the
interpreter, the `Session`, the screen, and back - and understand how a
script waits for something and resumes.

## Read

* `docs/SCRIPT_VM.md`: "Execution model", "A real script", and "The context
  status word, and what resumes a parked script". Skip the opcode tables.
* `engine/src/script/ctxstatus.h` — a dozen lines: every state a running
  script can be in, and what releases it.
* `engine/src/script/interp.cpp`, the `op::UiOpen` arm of
  `Interpreter::resume` (search for `op::UiOpen`).
* `engine/src/script/area.cpp`: the `case RunStatus::UiOpen:` arm, then
  `Session::answerUi` and `Session::uiAnswered`.

## What a world script is

The game's logic - what happens when you walk somewhere, use something, talk
to someone - is bytecode in `IAM\AREA`, `IAM\SCENE` and `IAM\GLOBAL`, run by a
small stack machine of 153 opcodes. Each area chunk carries records: trigger
zones on the ground, each with scripts for *enter*, *activate* (the action
button) and *leave*, plus a startup script and message subscriptions.

A script runs in a **context**: a pc, a stack, and a status word. Most
instructions finish at once. A few start something that takes time - a
conversation, a camera move, a scripted walk, a screen - and **park** the
context: its status says what it waits for, its pc already points past the
instruction, and the game's event that ends the wait sets it running again.
The rest of the world keeps going meanwhile.

## Run

### 1. The script

```
python3 tools/script_dump.py AREA 180 | sed -n 89,140p
```

AREA 180 holds the archives terminal. Its record 5, the script run when you
use the terminal at address 565, reads (cut):

```
  record 5 slot +4  @0x1181  [ok]
     4481  player.anim.hold
     4482  fade.to_black
     ...
     4507  fade.from_black
     4508  ui.open          18, -1, 19       ; SCREEN[18] = 'ARCHIVES'
     4515  fade.to_black
     4516  scx.play.player  obj 0x00d0, 0
     4521  inventory.remove_all 2, 337           ; OBJECTS[337] = 'Memo 004 Aller aux Archives'
     4526  push.i8          0
     4528  push.var         4                ; VARIABLES[4] = '1-A-CS Archives'
     4531  cmp.eq
     4532  jmp_if_false     59
     4535  push.i8          1
     4537  push.var         19               ; VARIABLES[19] = 'Interface'
     4540  cmp.eq
     4541  jmp_if_false     50
     4544  scene.load       35, 9            ; AREAS[35] = 'Anekbah Morgue' | SCENES[9] = '1-13 Morgue'
     ...
     4564  address.enable   3                ; ADDRESSES[3] = 'Anekbah - Morgue zone 42'
     ...
     4576  inventory.add    2, 339           ; OBJECTS[339] = 'Memo 006 Aller à la Morgue'
     4581  media.play       534              ; OBJECTS[534] = 'ZVO P315 DATA MEMORIZED'
     ...
     4617  actor.goto_address 565              ; ADDRESSES[565] = 'Terminal Archives'
     ...
     4632  end
```

Read it as the game's designer wrote it: hold the player, fade in, **open
screen 18 (the ARCHIVES) and put its answer in variable 19**; then fade,
take the "go to the archives" memo, and - if the archives were not already
done (variable 4 is 0) AND the screen answered 1 (the right dossier was
read) - load the morgue and the bars into the story, enable the morgue's
address, give the next memos and say *data memorised*. Every name after a
`;` is resolved from the game's own `.TAG` name tables.

### 2. The run

Walk to the terminal, use it, open the third dossier (*Meurtres*), scroll
it, go back, and close the screen - the keys the check `engine: dossier
reader` uses:

```
cd engine
H="k*40,k28*2,k*80,k205*2,k*4,k205*2,k*10,k28*2,k*20"
H="$H,k208*2,k*4,k208*2,k*4,k208*2,k*4,k208*2,k*4,k208*2,k*4,k208*2,k*4,k208*2,k*4"
H="$H,k*20,k57*2,k*20,k15*2,k*150"
SDL_VIDEODRIVER=dummy build/omk-play ../gamedata ../tables --software \
    --save ../traces/save-appart.bin --area 180 --address 565 \
    --frames 460 --nofmv --nodelay --no-crowd --hold "$H" > /tmp/arch.txt
grep -a 'screen 18 is\|answered\|DATA MEMORIZED\|ENABLED\|goto_address' /tmp/arch.txt
```

(One argument, `"$H"`, in quotes: the whole stream is one flag's value.)

```
frame 104: screen 18 is asking - arrows move, ENTER confirms, TAB closes
screen 18 answered 1 -> the script resumes (variable 19 now 1)
media.play 534 (ZVO P315 DATA MEMORIZED) -> JINGOFF3.ADP [JINGOFF3 substitution], 2.08 s
frame 310: ADDRESS 3 ENABLED - a place the scripts can send him now
frame 310: ADDRESS 336 ENABLED - a place the scripts can send him now
frame 380: actor.goto_address 565 - the player put down at 6848 476 -10977 facing 0
```

From frame 104 to frame 309 the script is **parked** on `ui.open`, and the
world goes on: the terminal's banner animates, the keys scroll the dossier.
When the screen closes, the answer arrives, the script resumes at 4515 and
runs the branch: the voice, the addresses, the walk back to 565.

### 3. The same instruction in the port

Follow `ui.open` through the code:

1. **`Interpreter::resume`** (`src/script/interp.cpp`) decodes the
   instruction's three operands - screen, parameter, result variable - and
   **returns** `RunStatus::UiOpen` with `pc` already past the instruction.
   The interpreter does not wait; it tells its caller what it is waiting for.
2. **The `RunStatus::UiOpen` arm** in `src/script/area.cpp` sets the
   context's status to `ctx_status::WaitScreen` (6) and, when a person will
   answer, records the request in `pendingUi_`: the screen, its parameter,
   the variable, the context. Then it returns, and the `Session` runs every
   other context as usual, frame after frame.
3. **The viewer** sees `Session::pendingUiScreen()` become 18, opens the
   screen, and walks it with your keys (lesson 6 is that walk).
4. **`Session::answerUi`** is `Game_HandleEvent` case 5 in the original:
   write the answer into the variable the `ui.open` named, then
   `Session::uiAnswered` sets the context back to `Running`. Next frame the
   pump resumes it at 4515.

## Change

Make the archives forget what they were told. In `Session::answerUi`, the
line that writes the variable:

```cpp
if (pendingUi_.var >= 0) state_.setVar(pendingUi_.var, value);
```

write `0` instead of `value`. Rebuild (`make play`) and run the same command:

```
screen 18 answered 1 -> the script resumes (variable 19 now 0)
frame 380: actor.goto_address 565 - the player put down at 6848 476 -10977 facing 0
```

The screen still answered 1 - that line prints the screen's answer, and then
the variable as the script will read it. The script resumes, `push.var 19`
gives 0, `jmp_if_false 50` jumps over the whole branch: no memo, no voice,
no addresses. The story does not move on. Only the code after the branch -
the camera, the walk back to 565 - still runs.

## Prove

```
cd ..
python3 tools/verify.py --only "engine: dossier reader"
```

With the change in, it fails - its second value is `ADDRESS 3 ENABLED` and
it went `False`:

```
engine: dossier reader FAIL ((..., 'answered 1'), False, (...)) != ((..., 'answered 1'), True, (...))
```

Put `value` back, `touch src/script/area.cpp`, rebuild, and run it green.

## Traps

* **A scene-local id means nothing outside its scene.** `scx.play.player
  obj 0x00d0` names an object of the RESIDENT scene, and every scene numbers
  its own from zero. Counting a number across the whole script corpus counts
  collisions.
* **Two functions of one family need not share a parameter layout.** In one
  sound opcode the second operand is the frame to fire on, in its sibling a
  loop flag. Decoding them alike invents a timing for every call of the
  second.
* **An operand length you change is tested together with every other.** The
  opcode table's lengths are corpus-tested - a wrong length desynchronises
  every instruction after it. But while one length is wrong, a test of
  another is testing both: an assembly that is unambiguous wins over a corpus
  that disagrees, and the disagreement is a symptom to locate.
