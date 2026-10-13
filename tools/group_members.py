#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Gather a run of a class's fields into a nested struct - an owned component.

`todo/code-readability-audit.md` §6: `PlayState` holds ~900 members, and
nothing shows which belong together or who reads them. A run of fields under
one banner that is read and written as one thing becomes

    struct SliderCam { int mode{-1}; float clock{}, ...; } sliderCam;

and every `sliderCamMode` in the class's files becomes `sliderCam.mode`:

    python3 tools/group_members.py <header.h> A-B[,C-D...] <Type> <member> <prefix> [--build T...]

The header's lines in those ranges are scanned for FIELD declarations whose names all
start with `prefix` (a line mixing other names is refused); the comment lines
among them go with the struct, method declarations stay where they were. A
name loses the prefix and has its first letter lowered (`sliderCamFromEye`
-> `fromEye`); a clash refuses. References are rewritten in code - not after
`.`, `->` or `::` - and backticked mentions in comments, in the header and in
every .cpp that defines one of the class's members (a field of the same name
in another class is left alone). Then `make` builds the targets named (`play`
by default); a failure restores every file. `verify.py` lines that name an old field are printed:
a source-scanning check may need the new spelling.
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG = os.path.join(ROOT, "engine")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_method import code_mask   # noqa: E402


def field_names(line, prefix):
    """The declarator names of a field line, or None if it is not one."""
    code = line.split("//", 1)[0].rstrip()
    if not code.endswith(";") or "(" in re.sub(r"\{[^}]*\}", "", code):
        return None
    body = code[:-1]
    m = re.match(r"^\s*(?:static\s+|mutable\s+|const\s+)*([\w:]+(?:\s*<[^;]*>)?)\s+(.*)$", body)
    if not m:
        return None
    decls = re.sub(r"\{[^}]*\}|\[[^\]]*\]", "", m.group(2))   # braces first: `= {0, 0}`
    decls = re.sub(r"=[^,]*", "", decls)
    names = [d.strip().lstrip("*&").strip() for d in decls.split(",")]
    if not all(re.fullmatch(r"\w+", n) for n in names):
        return None
    return names


def main(a):
    if len(a) < 5:  # noqa
        print(__doc__)
        return 2
    hdr, spec, typ, member, prefix = a[0], a[1], a[2], a[3], a[4]
    lines = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        lines += list(range(int(lo) - 1, int(hi or lo)))
    A, B = min(lines) + 1, max(lines) + 1
    targets = a[a.index("--build") + 1:] if "--build" in a else ["play"]
    H = open(hdr).read().split("\n")
    fields, comments, keep = [], [], [i for i in range(A - 1, B) if i not in lines]
    for i in lines:
        line = H[i]
        names = field_names(line, prefix)
        if line.strip().startswith("//") and not [k for k in keep if k > A - 1 and k < i]:
            comments.append(line)
        elif names is not None:
            if not all(n.startswith(prefix) and len(n) > len(prefix) for n in names):
                print("refused: line %d mixes names outside `%s`: %s" % (i + 1, prefix, line.strip()))
                return 1
            fields.append((i, line, names))
        else:
            keep.append(i)            # a method, or a comment over something that stays
    if not fields:
        print("refused: no field starting with `%s` in lines %d-%d" % (prefix, A, B))
        return 1
    cls = next((m.group(2) for k in range(A - 1, -1, -1)
                for m in [re.match(r"^(struct|class) (\w+)\b[^;]*$", H[k])] if m), None)
    if not cls:
        print("refused: no class or struct encloses line %d" % A)
        return 1
    rename = {}
    for _, _, names in fields:
        for n in names:
            new = n[len(prefix)]
            new = new.lower() + n[len(prefix) + 1:]
            if n.endswith("_"):
                new = new.rstrip("_")    # a class's `x_` is a struct's `x`; the member keeps the `_`
            if new in rename.values() or not re.fullmatch(r"[a-z_]\w*", new):
                print("refused: %s -> %s clashes or is not a name" % (n, new))
                return 1
            rename[n] = new
    # the struct, at the first field's place; comments that headed the run go above it
    ind = len(fields[0][1]) - len(fields[0][1].lstrip())
    body = []
    for i in range(A - 1, B):
        if i in keep:
            continue
        line = H[i]
        if any(i == f[0] for f in fields):
            for old, new in rename.items():
                line = re.sub(r"(?<![\w.])%s\b" % re.escape(old), new, line)
        body.append("    " + line)
    struct = [" " * ind + "struct %s {" % typ] + body + [" " * ind + "} %s{};" % member]
    first = min(f[0] for f in fields)
    out = H[:first] + struct + [H[i] for i in range(first, B) if i in keep] + H[B:]
    # what reaches into the header before the run's first line (its banner) stays put
    files = [hdr]
    cls_files = set()
    for f in [g for d in ("src", "backends", "tools") for ext in ("cpp", "h")
              for g in glob.glob(os.path.join(ENG, d, "**", "*." + ext), recursive=True)]:
        if "third_party" in f or os.path.abspath(f) == os.path.abspath(hdr):
            continue
        t = open(f, errors="replace").read()
        # only the class's own files: the .cpp files that DEFINE its members (a
        # header naming it may be another class's, with a field of the same name)
        defines = re.search(r"^[\w:<>,*& ]*\b%s::~?\w+\([^;]*$" % re.escape(cls), t, re.M)
        if f.endswith(".cpp") and defines and \
                any(re.search(r"(?<![\w.>:])%s\b" % re.escape(o), t) for o in rename):
            cls_files.add(f)
    originals = {f: open(f).read() for f in [hdr] + sorted(cls_files)}
    try:
        open(hdr, "w").write("\n".join(out))
        for f in sorted(cls_files) + [hdr]:
            t = open(f).read()
            m = code_mask(t)
            res, last = [], 0
            pat = re.compile(r"(?<![\w.>:])(%s)\b" % "|".join(re.escape(o) for o in rename))
            for mm in pat.finditer(t):
                k = mm.start()
                if not m[k]:
                    # in a comment or a string: only a backticked mention moves
                    if t[k - 1:k] == "`" and t[mm.end():mm.end() + 1] == "`":
                        res.append(t[last:k] + "%s.%s" % (member, rename[mm.group(1)]))
                        last = mm.end()
                    continue
                if f == hdr and t[:k].count("\n") >= first and \
                        t[:k].count("\n") < first + len(struct):
                    continue                 # the struct's own (already renamed) fields
                res.append(t[last:k] + "%s.%s" % (member, rename[mm.group(1)]))
                last = mm.end()
            res.append(t[last:])
            open(f, "w").write("".join(res))
        r = subprocess.run(["make", "-s"] + targets, cwd=ENG, capture_output=True, text=True)
        errs = [l for l in (r.stdout + r.stderr).split("\n") if ": error: " in l]
        if r.returncode != 0 or errs:
            raise RuntimeError("\n  ".join(errs[:8]) or "make failed")
    except Exception as e:
        for f, t in originals.items():
            open(f, "w").write(t)
        print("refused, restored: %s" % e)
        return 1
    v = open(os.path.join(ROOT, "tools", "verify.py")).read().split("\n")
    hits = [(k + 1, l.strip()[:100]) for k, l in enumerate(v)
            if any(re.search(r"\b%s\b" % re.escape(o), l) for o in rename)]
    print("%s %s: %d fields in %d files" % (typ, member, len(rename), len(cls_files) + 1))
    for k, l in hits:
        print("  verify.py:%d names an old field: %s" % (k, l))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
