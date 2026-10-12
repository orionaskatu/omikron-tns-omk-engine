#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Prove that a change to C++ files touched COMMENTS ONLY.

`todo/code-readability-audit.md` step 3 rewrites hundreds of comments, and a
slip in one of them - a `*/` dropped, a line of code swallowed into a `//` -
compiles more often than not. This strips every comment and every run of
whitespace from each changed `.cpp`/`.h`, in the working tree and at a git
revision, and compares the two token streams. Any difference is a CODE
change and is printed with its context.

    python3 tools/comments_only.py            # against HEAD
    python3 tools/comments_only.py <rev>      # against another revision

Exit status 0 when every changed file's code is identical. String and
character literals are kept verbatim (a `//` inside a string is not a
comment), and a raw string literal is not expected in this tree.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    # whitespace is not code; line splits and indentation may move freely
    return re.sub(r"\s+", " ", "".join(out)).strip()


def main(argv):
    rev = argv[0] if argv else "HEAD"
    names = subprocess.run(["git", "diff", "--name-only", rev, "--", "*.cpp", "*.h"],
                           cwd=ROOT, capture_output=True, text=True).stdout.split()
    bad = 0
    for f in names:
        p = os.path.join(ROOT, f)
        if not os.path.exists(p):
            print("%s: deleted" % f); bad += 1; continue
        old = subprocess.run(["git", "show", "%s:%s" % (rev, f)], cwd=ROOT,
                             capture_output=True, text=True, errors="replace")
        if old.returncode != 0:
            print("%s: new file" % f); bad += 1; continue
        a, b = strip(old.stdout), strip(open(p, errors="replace").read())
        if a != b:
            bad += 1
            k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            print("%s: CODE CHANGED near ...%s...\n    was ...%s..." %
                  (f, b[max(0, k - 60):k + 60], a[max(0, k - 60):k + 60]))
    print("%d file(s) changed, %d with code changes" % (len(names), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
