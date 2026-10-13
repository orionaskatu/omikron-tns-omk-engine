# Working on OMK by hand — a course

This is a hands-on course for **changing OMK yourself**: finding where a
behaviour lives, reading the original engine to know what it should be,
making the change, watching it in `omk-play`, and proving it with
`verify.py`. Each lesson is one sitting, and every exercise is a real edit
you make, build and then undo.

It is **not** the book. `book/` tells the engine and the port in order, for
reading. This course is for doing: it sends you into the code, says exactly
what to run, and shows the output you should get.

**Who it is written for:** someone who knows C++ (classes, templates,
lambdas, the standard library) and a shell, and nothing about Omikron, its
engine or this repository. You will not need Windows, the original game
running, or a disassembler: the lessons that read the original use the
listing through `tools/asmfn.py` and the hand-cleaned `readable/` tree.
*(The level was the one question left open when the course was planned; it
was settled by default, and is easy to change if it is wrong for you.)*

## How a lesson is built

Every lesson has the same six parts:

| part | what it gives you |
|---|---|
| **Goal** | one sentence: what you can do at the end |
| **Read** | only the files and functions this lesson needs, and in what order |
| **Run** | the exact commands, and the lines of output to look for |
| **Change** | a small edit you make yourself |
| **Prove** | the check that covers it - run it green, break the code on purpose, watch it go red, restore |
| **Traps** | the mistakes that have already cost time on this subject |

The outputs printed in the lessons were produced by running the commands on
the tree as it is. If yours differ, something changed - which is itself
worth understanding before going on.

Lessons name **functions and files, not line numbers**: lines move with every
commit, names rarely do. `verify.py: tutorial anchors` checks that every
path and every function a lesson names still exists, so the course cannot
quietly go stale.

## The lessons

| # | lesson | you will |
|---|---|---|
| 0 | [Setup](00-setup.md) | point the repo at your game, build, boot the replica, and see it agree with the original 42 decisions of 42 |
| 1 | [The tree, and one key press](01-the-tree.md) | know where each subsystem lives, and follow the UP arrow from SDL to the walker |
| 2 | [A file format](02-a-file-format.md) | write a small tool on the texture reader, with the output-path guard, and break the codec to see the whole-corpus check catch it |
| 3 | [Reading the original](03-reading-the-original.md) | find a function by name, by address and in the executable, know which to trust, and find its port - and a behaviour no check covers |
| 4 | [The script VM](04-the-script-vm.md) | follow `ui.open` from a world script through the interpreter, the `Session` and the screen, and break the answer it brings back |
| 5 | [The frame loop](05-the-frame-loop.md) | the frame's clock and phases, measured; an `@evt` line a check can read, invisible to every other |
| 6 | The interface walk | change what a screen answers and watch a check fail for it |
| 7 | Bodies | the `.CTL` channel and the player controller: change a threshold, see the walk change |
| 8 | The renderer boundary | the software reference against a GPU backend; compare two frames |
| 9 | Writing a check | build the right binaries, print what the consumer produced, show it fail |
| 10 | The traps | reproduce three of the repository's classic mistakes on purpose |
| 11 | Capstone | port one small thing end to end |

Lessons 2 to 11 arrive in batches; the table links each one as it is written.

## Four rules before you start

1. **`gamedata/` is input. Never write into it.** Every tool that writes a
   file checks its output path (`omk::safeOutputPath`); a tool you write in
   lesson 2 will too. The data came off a disc and has no other copy.
2. **A headless run sets `SDL_VIDEODRIVER=dummy`.** Without it `omk-play`
   opens a real window that takes the keyboard, even with `--frames`.
3. **This is zsh.** A variable holding several flags is ONE argument unless
   you write `${=VAR}`. When a command is built from a variable, check what
   it did (a file written, a line printed), not that it exited 0.
4. **Undo an exercise by editing the line back**, or with
   `git checkout -- <file>` only when that file holds nothing else of yours.
   Then `touch` it and rebuild: macOS `make` compares times to the second,
   and a restore in the same second as the last build is invisible to it.

`dump2png.py` beside this file turns an `omk-play --dump` frame into a PNG.
