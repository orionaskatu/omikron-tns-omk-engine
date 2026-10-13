#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn an `omk-play --dump` frame into a PNG you can open.

    python3 tutorial/dump2png.py frame.bin [frame.png] [--size 640x480]

A dump is the framebuffer as it was: little-endian RGB565, two bytes a pixel,
row after row, no header - so its size alone gives the picture's area, and a
`--res` other than 640x480 needs `--size` to say how wide a row is. Writes
next to the dump when no output is named. Stdlib only (`tools/frame.py`'s
PNG writer), like every tool in this repo.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from frame import write_png  # noqa: E402


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    w, h = 640, 480
    if "--size" in argv:
        w, h = (int(v) for v in argv[argv.index("--size") + 1].split("x"))
        args = [a for a in args if a != argv[argv.index("--size") + 1]]
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + ".png"
    raw = open(src, "rb").read()
    if len(raw) != 2 * w * h:
        print("%s is %d bytes, not %dx%d RGB565 (%d) - pass --size WxH" % (src, len(raw), w, h, 2 * w * h))
        return 1
    rgb = bytearray(3 * w * h)
    for i in range(w * h):
        p = raw[2 * i] | raw[2 * i + 1] << 8
        r, g, b = p >> 11, (p >> 5) & 63, p & 31
        rgb[3 * i] = (r << 3) | (r >> 2)          # 5 and 6 bits widened to 8
        rgb[3 * i + 1] = (g << 2) | (g >> 4)
        rgb[3 * i + 2] = (b << 3) | (b >> 2)
    write_png(dst, w, h, bytes(rgb))
    print("wrote %s (%dx%d)" % (dst, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
