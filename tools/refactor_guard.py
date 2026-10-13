#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""The REFACTOR GUARD: proof that a change moved no behaviour.

`todo/code-readability-audit.md` asks for a long series of changes that must
change NOTHING the game does - names given to numbers, giant functions split
along their own banners, helpers shared. `verify.py` asserts on the viewer's
stdout, so the strongest cheap proof is that the stdout itself did not move:
this records the WHOLE output of a fixed set of runs - the boot, the menu, the
street with its crowd, the terminals, a fight, shoot mode, a slider, a boss,
the sneak, the puzzle screens, a set render - together with a hash of each
run's last frame, and diffs a recording against another.

    python3 tools/refactor_guard.py record base      # before the change
    python3 tools/refactor_guard.py record now       # after it
    python3 tools/refactor_guard.py diff base now    # nothing printed = identical

Recordings go under `engine/build/refguard/<label>/` (build output, never
committed). Lines that measure the HOST - timings, frame rates, addresses - are
dropped before the comparison; the list is `VOLATILE` below and was found by
recording the same tree twice. A run that differs prints its first differing
lines, so a real change is visible at once.

It is a guard for behaviour-preserving work only. A change that is MEANT to
move behaviour is proved by its own check, as always (`docs/PORTING.md` B2).
"""
import concurrent.futures
import difflib
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import omkpaths

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG = os.path.join(ROOT, "engine")
OUT = os.path.join(ENG, "build", "refguard")
SAVE = os.path.join(ROOT, "traces", "save-appart.bin")

# Lines that measure the machine rather than the game. Found by recording the
# same tree twice (`record a`, `record b`, `diff a b`) and naming every line
# that moved.
VOLATILE = [
    re.compile(r"\d ?ms\b|\(ms\b"),                # any timing in milliseconds
    re.compile(r"music peak"),                       # the mixer thread's level
    re.compile(r"\b\d+(\.\d+)? ?(ms|us|µs|s)\b.*(frame|took|in |boot|load|wall)", re.I),
    re.compile(r"\bfps\b", re.I),
    re.compile(r"0x[0-9a-f]{8,16}\b"),            # host pointers
    re.compile(r"(elapsed|wall clock|presented in|seconds)", re.I),
    re.compile(r"/private/|/tmp/|refguard/"),       # scratch paths
]

# name -> (extra viewer args, extra env). Every run is the software renderer
# under the dummy video driver, at 640x480, with a private empty saves file.
SCENARIOS = {
    "menu": (["--frames", "400", "--nofmv"], {}),
    "street": (["--save", SAVE, "--area", "0", "--stand", "1804,0,-6890,336",
                "--frames", "300", "--nofmv"], {}),
    "terminal": (["--save", SAVE, "--area", "179", "--stand", "5468,336,-12118,120",
                  "--frames", "420", "--nofmv", "--nodelay", "--no-crowd", "--hold",
                  "k*40,k28*2,k*300,k208*2,k*4,k28*2,k*20,k208*2,k*4,k208*2,k*20,"
                  "k57*2,k*20,k15*2,k*40"], {}),
    "archives": (["--save", SAVE, "--area", "180", "--address", "565", "--frames", "460",
                  "--nofmv", "--nodelay", "--no-crowd", "--hold",
                  "k*40,k28*2,k*80,k205*2,k*4,k205*2,k*10,k28*2,k*20" + ",k208*2,k*4" * 7 +
                  ",k*20,k57*2,k*20,k15*2,k*150"], {}),
    "fight": (["--save", SAVE, "--fight-supermarket", "--frames", "1080"],
              {"OMK_CAMLOG": "1"}),
    "shoot": (["--save", SAVE, "--area", "230", "--scene-chunk", "56", "--frames", "700",
               "--nodelay"], {}),
    "board": (["--save", SAVE, "--area", "0", "--stand", "1804,0,-6890,244",
               "--frames", "900", "--board", "--nofmv"], {"OMK_CAMEYE": "1"}),
    "astaroth": (["--save", SAVE, "--area", "175", "--address", "526", "--zone-enable",
                  "2936", "--zone-disable", "2935", "--hide-show", "34,1,100000,0",
                  "--frames", "500", "--nodelay", "--shoot-health", "1000",
                  "--keys", ",".join(["54"] * 6), "--keydelay", "40"], {}),
    "sneak": (["--save", SAVE, "--area", "0", "--stand", "1804,0,-6890,336", "--nofmv",
               "--no-crowd", "--give", "2:913,2:915,2:336", "--frames", "960", "--keys",
               "0x0F,0x0F,0xCD,0xD0,0x1C,0xCD,0xD0,0xD0,0x1C,0xD0,0xD0,0xD0",
               "--keydelay", "60"], {}),
    # ends at 600, INSIDE the conversation the 4000 ms delayed close opens
    # (frame 468): its scroll arrows pulse on the interface clock, a headless
    # run's frames, so the last frame is one picture every run (it stopped at
    # 460 while they pulsed on the wall's - todo/code-readability-audit.md 6b)
    "xachen": (["--save", SAVE, "--area", "58", "--var", "28=1,57=0", "--stand",
                "-1471,150,133,290", "--frames", "600", "--nofmv", "--nodelay", "--no-crowd",
                "--hold", ",".join(["k*60", "k28*8", "k*60"] + ["k28*2,k*5"] * 7 +
                                   ["k205*2,k*5"] + ["k28*2,k*5"] * 3 + ["k205*2,k*5"] +
                                   ["k28*2,k*5"] * 10 + ["k205*2,k*5"] + ["k28*2,k*5"] * 5 +
                                   ["k*80"])], {}),
    "gandhar-door": (["--save", SAVE, "--area", "81", "--address", "263", "--frames", "400",
                      "--nofmv", "--nodelay", "--no-crowd", "--hold",
                      "k*50,k28*3,k*40" + ",k28*3,k*20" * 4 + ",k205*3,k*100"], {}),
    "set": (["--scene", "Aapkayl", "--frames", "4"], {}),
}


# PROBES - the engine's own tools, for what no viewer run reaches often enough.
# name -> (tool, args, the file it writes or None). `{out}` is the output path,
# `{data}` the game data root. ADDED the day a viewer-only guard went GREEN on a
# real change: an operator-precedence slip turned one channel test always-true,
# which disabled one blended-transition path - the thirteen runs never crossed
# it, and `run_actor_states`' 395352-tick sweep of every bank moved by 8.
PROBES = {
    "actor-states": ("run_actor_states", ["{data}/ANIMS", "{out}"], True),
}


def build():
    for target in ([], ["play"]):
        r = subprocess.run(["make", "-s"] + target, cwd=ENG, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("make %s failed:\n%s" % (" ".join(target) or "(engine)", r.stderr[-2000:]))


def scrub(text):
    return "\n".join(l for l in text.splitlines()
                     if not any(p.search(l) for p in VOLATILE)) + "\n"


def run_one(label, name, args, env):
    d = os.path.join(OUT, label)
    dump = os.path.join(d, name + ".bin")
    saves = os.path.join(d, name + ".saves")
    if os.path.exists(saves):
        os.remove(saves)
    cmd = [os.path.join(ENG, "build", "omk-play"), omkpaths.data_root(),
           os.path.join(ROOT, "tables"), "--software", "--res", "640x480",
           "--saves", saves, "--dump", dump] + args
    r = subprocess.run(cmd, capture_output=True, errors="replace",
                       env=dict(os.environ, SDL_VIDEODRIVER="dummy", **env), text=True)
    frame = hashlib.sha1(open(dump, "rb").read()).hexdigest() if os.path.exists(dump) else "none"
    body = scrub(r.stdout) + "---- stderr\n" + scrub(r.stderr) + \
        "---- exit %d, last frame sha1 %s\n" % (r.returncode, frame)
    with open(os.path.join(d, name + ".txt"), "w") as f:
        f.write(body)
    return name, len(body.splitlines())


def record(label, only=None):
    build()
    d = os.path.join(OUT, label)
    os.makedirs(d, exist_ok=True)
    # the headless boot, which `verify.py: engine: intro` compares with the
    # original's own trace
    r = subprocess.run([os.path.join(ENG, "build", "omk"), omkpaths.data_root(),
                        "--tables", os.path.join(ROOT, "tables")],
                       capture_output=True, text=True, errors="replace")
    with open(os.path.join(d, "boot.txt"), "w") as f:
        f.write(scrub(r.stdout) + "---- exit %d\n" % r.returncode)
    for name, (tool, args, writes) in PROBES.items():
        if only and name not in only:
            continue
        out = os.path.join(d, name + ".bin")
        if os.path.exists(out):
            os.remove(out)
        argv = [os.path.join(ENG, "build", tool)] + \
            [x.format(out=out, data=omkpaths.data_root()) for x in args]
        r = subprocess.run(argv, capture_output=True, text=True, errors="replace")
        h = hashlib.sha1(open(out, "rb").read()).hexdigest() \
            if writes and os.path.exists(out) else "none"
        with open(os.path.join(d, name + ".txt"), "w") as f:
            f.write(scrub(r.stdout) + "---- exit %d, output sha1 %s\n" % (r.returncode, h))
        print("  %-13s probe, output %s" % (name, h[:12]))
    todo = [(n, a, e) for n, (a, e) in SCENARIOS.items() if not only or n in only]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        for name, n in ex.map(lambda t: run_one(label, *t), todo):
            print("  %-13s %6d lines" % (name, n))
    print("recorded %d runs + the boot into %s" % (len(todo), d))


def diff(a, b):
    da, db = os.path.join(OUT, a), os.path.join(OUT, b)
    names = sorted(f for f in os.listdir(da) if f.endswith(".txt"))
    bad = 0
    for f in names:
        pb = os.path.join(db, f)
        if not os.path.exists(pb):
            print("%s: missing from %s" % (f, b)); bad += 1; continue
        # scrubbed again, so a recording taken before a VOLATILE entry was
        # added still compares
        la = scrub(open(os.path.join(da, f)).read()).splitlines()
        lb = scrub(open(pb).read()).splitlines()
        if la != lb:
            bad += 1
            print("== %s differs:" % f)
            for k, line in enumerate(difflib.unified_diff(la, lb, a, b, n=1, lineterm="")):
                if k > 24:
                    print("   ..."); break
                print("   " + line)
    print("%d of %d runs differ" % (bad, len(names)))
    return bad


def main(argv):
    if len(argv) >= 2 and argv[0] == "record":
        record(argv[1], set(argv[2:]) or None)
        return 0
    if len(argv) == 3 and argv[0] == "diff":
        return 1 if diff(argv[1], argv[2]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
