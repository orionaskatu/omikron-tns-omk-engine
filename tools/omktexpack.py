#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""THE TEXTURE PACK PIPELINE - the remaster's first feature (todo/remaster.md B).

Makes the directory `omk-play --texture-pack DIR` (or `[Remaster] texturepack`)
reads: one `<key>.png` per texture, RGBA, the original's size times a power of
two each way, the key the engine's `textureKey` (engine/src/o3de/texpack.h).

    omktexpack.py export  OUT [--match S ...]     the shipped textures, ready to upscale
    omktexpack.py upscale OUT --bin PATH [--model M] [--tile N]
                                                  run Real-ESRGAN (ncnn-vulkan) on them
    omktexpack.py build   OUT [--no-anchor] [--grain F] [--nearest N | --solid R,G,B N] [--match S ...]
                                                  -> OUT/pack, the directory to hand the viewer

`export` writes, under OUT:
    index.json        key -> {name, model, material, w, h, keyed}
    src/<key>.png     the texture as shipped (RGB), for looking at
    in/<key>.png      the UPSCALER'S input: wrapped by PAD texels on every
                      side (the engine REPEATS textures, so an edge must be
                      upscaled knowing what is across it - no seams), and the
                      colour-key texels (pure black, cut out on a flag-0x800
                      mesh) filled from their neighbours, so the upscaler does
                      not smear black into the edge
    in/<key>_mask.png the key as white/black, padded the same way - upscaled
                      too, and thresholded, so a cutout's edge comes out
                      smooth instead of in 4x4 steps
`upscale` runs the binary over in/ into up/ (it keeps the names).
`build` crops the pad back off, puts the key back in alpha (colour 0 where it
is 0, as `Texture::toRgbaKeyed` does) and writes pack/<key>.png. `--grain F`
(0..1) mixes a plain bilinear enlargement of the original back in at F: the
upscalers DENOISE, and the painted grain of a floor is part of the art (the
pilot of 2026-10-20 - todo/remaster.md). By DEFAULT the result is ANCHORED to
the shipped texels (back-projection, `anchor` below): its box average at the
original's size is the original again, so the grain and the painted colours
stay the artists' and the upscaler adds only sub-texel sharpness;
`--no-anchor` keeps the upscaler's raw picture. `--nearest N`
and `--solid` build from the ORIGINALS instead: the first is the shipped
picture at N times the texels (the engine must draw it identically - the
check's invariant), the second one colour (the engine must visibly draw it).

NOTHING THIS WRITES MAY BE COMMITTED: the pixels are the game's, the repo is
public. So OUT is refused inside the repository and inside the data tree.
Stdlib only; the PNG READING of an upscaler's output goes through the engine's
own reader (`engine/build/texpack_tool png`), because pure-Python un-filtering
of a 3072x3072 image takes tens of seconds.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import omkpaths  # noqa: E402
import tex3dt    # noqa: E402

ROOT = os.path.dirname(HERE)
PAD = 16
TOOL = os.path.join(ROOT, "engine", "build", "texpack_tool")


# ---- the key: FNV-1a 64, exactly `textureKey` ------------------------------
def texture_key(w, h, idx, pal):
    hsh = 0xcbf29ce484222325
    data = struct.pack("<II", w, h) + idx[:w * h] + pal[:768].ljust(768, b"\0")
    for b in data:
        hsh ^= b
        hsh = (hsh * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % hsh


# ---- PNG, written (RGB or RGBA, filter 0) ------------------------------------
def write_png(path, w, h, px, channels):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    stride = w * channels
    raw = b"".join(b"\0" + px[y * stride:(y + 1) * stride] for y in range(h))
    ctype = 2 if channels == 3 else 6
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6))
                + chunk(b"IEND", b""))


def read_png(path):
    """-> (w, h, rgba) through the engine's reader."""
    if not os.path.exists(TOOL):
        sys.exit(f"{TOOL} is missing - run `make` in engine/ first")
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "x.raw")
        r = subprocess.run([TOOL, "png", path, out], capture_output=True, text=True)
        if r.returncode != 0:
            raise ValueError(r.stderr.strip() or f"cannot read {path}")
        raw = open(out, "rb").read()
    w, h = struct.unpack_from("<ii", raw, 0)
    return w, h, raw[8:]


# ---- where we may write ------------------------------------------------------
def safe_out(out):
    o = os.path.realpath(out)
    for bad, why in ((os.path.realpath(ROOT), "the repository (the pixels must never be committed)"),
                     (os.path.realpath(omkpaths.data_root()), "the game data (it is input only)")):
        if o == bad or o.startswith(bad + os.sep):
            sys.exit(f"refusing to write inside {why}: {out}")
    return o


# ---- the shipped textures ----------------------------------------------------
def shipped(match=()):
    """-> {key: entry} over every .3DO/.3DT pair under MESHES, first model wins."""
    data = omkpaths.data_root()
    meshes = next((os.path.join(data, d) for d in os.listdir(data) if d.lower() == "meshes"), None)
    if not meshes:
        sys.exit(f"no MESHES under {data}")
    out = {}
    models = []
    for dp, _dn, fn in os.walk(meshes):
        for f in fn:
            if f.lower().endswith(".3do"):
                models.append(os.path.join(dp, f))
    for m in sorted(models):
        rel = os.path.relpath(m, data).replace(os.sep, "/")
        if match and not any(s.lower() in rel.lower() for s in match):
            continue
        for i, tx in enumerate(tex3dt.textures(m)):
            w, h = tx["w"], tx["h"]
            if w <= 0 or h <= 0:
                continue
            k = texture_key(w, h, tx["idx"], tx["pal"])
            if k in out:
                continue
            out[k] = {"name": tx["name"], "model": rel, "material": i, "w": w, "h": h,
                      "rgb": tx["rgb"]}
    return out


def key_mask(rgb, n):
    """1 per texel the engine KEYS (pure black), else 0 - `toRgbaKeyed`'s rule."""
    return bytes(1 if rgb[3 * i] == 0 and rgb[3 * i + 1] == 0 and rgb[3 * i + 2] == 0 else 0
                 for i in range(n))


def dilate(rgb, keyed, w, h, passes=PAD):
    """Fill keyed texels from unkeyed neighbours (wrapping), up to `passes` deep."""
    px = bytearray(rgb)
    known = bytearray(1 - k for k in keyed)
    for _ in range(passes):
        grow = []
        for i in range(w * h):
            if known[i]:
                continue
            x, y = i % w, i // w
            acc = [0, 0, 0]; c = 0
            for j in (y * w + (x - 1) % w, y * w + (x + 1) % w,
                      ((y - 1) % h) * w + x, ((y + 1) % h) * w + x):
                if known[j]:
                    acc[0] += px[3 * j]; acc[1] += px[3 * j + 1]; acc[2] += px[3 * j + 2]; c += 1
            if c:
                grow.append((i, acc[0] // c, acc[1] // c, acc[2] // c))
        if not grow:
            break
        for i, r, g, b in grow:
            px[3 * i:3 * i + 3] = bytes((r, g, b)); known[i] = 1
    return bytes(px)


def padded(px, w, h, ch, pad=PAD):
    """The image wrapped by `pad` texels on every side."""
    rows = []
    for yy in range(-pad, h + pad):
        y = yy % h
        row = px[y * w * ch:(y + 1) * w * ch]
        left = b"".join(row[((x % w) * ch):((x % w) * ch) + ch] for x in range(-pad, 0))
        right = b"".join(row[((x % w) * ch):((x % w) * ch) + ch] for x in range(w, w + pad))
        rows.append(left + row + right)
    return b"".join(rows), w + 2 * pad, h + 2 * pad


def nearest(px, w, h, ch, n):
    """`px` with every texel an n x n block."""
    rows = []
    for y in range(h):
        row = px[y * w * ch:(y + 1) * w * ch]
        big = b"".join(row[x * ch:(x + 1) * ch] * n for x in range(w))
        rows.append(big * n)
    return b"".join(rows)


def bilinear(px, w, h, ch, n):
    """`px` n times larger, bilinear, WRAPPING (the engine repeats textures)."""
    def taps(size):
        out = []
        for o in range(size * n):
            f = (o + 0.5) / n - 0.5
            i0 = int(f // 1)
            t = int(round((f - i0) * 256))
            out.append((i0 % size, (i0 + 1) % size, 256 - t, t))
        return out
    tx, ty = taps(w), taps(h)
    W = w * n
    rows = []
    for y in range(h):
        r = px[y * w * ch:(y + 1) * w * ch]
        row = bytearray(W * ch)
        for o, (a, b, wa, wb) in enumerate(tx):
            for c in range(ch):
                row[o * ch + c] = (r[a * ch + c] * wa + r[b * ch + c] * wb + 128) >> 8
        rows.append(bytes(row))
    out = bytearray()
    for a, b, wa, wb in ty:
        ra, rb = rows[a], rows[b]
        out += bytes((x * wa + z * wb + 128) >> 8 for x, z in zip(ra, rb))
    return bytes(out)


def bilinear_signed(vals, w, h, ch, n):
    """`bilinear` over a list of signed ints (a residual), wrapping."""
    def taps(size):
        out = []
        for o in range(size * n):
            f = (o + 0.5) / n - 0.5
            i0 = int(f // 1)
            t = (f - i0)
            out.append((i0 % size, (i0 + 1) % size, 1.0 - t, t))
        return out
    tx, ty = taps(w), taps(h)
    W = w * n
    rows = []
    for y in range(h):
        r = vals[y * w * ch:(y + 1) * w * ch]
        rows.append([r[a * ch + c] * wa + r[b * ch + c] * wb
                     for (a, b, wa, wb) in tx for c in range(ch)])
    out = []
    for a, b, wa, wb in ty:
        ra, rb = rows[a], rows[b]
        out.extend(x * wa + z * wb for x, z in zip(ra, rb))
    return out


def anchor(up, src, w, h, n):
    """BACK-PROJECTION: correct the upscaled `up` (RGB, w*n x h*n) so that its
    n x n box average is the SHIPPED texel again, by adding the bilinear
    enlargement of the residual. The texel-scale content - the grain, the
    colours the artists painted - is then the original's; what the upscaler
    contributes is only what lies between texels (an edge's sharpness)."""
    W = w * n
    down = [0] * (w * h * 3)
    inv = 1.0 / (n * n)
    for y in range(h):
        for x in range(w):
            acc = [0, 0, 0]
            for yy in range(y * n, y * n + n):
                base = (yy * W + x * n) * 3
                seg = up[base:base + 3 * n]
                acc[0] += sum(seg[0::3]); acc[1] += sum(seg[1::3]); acc[2] += sum(seg[2::3])
            k = (y * w + x) * 3
            down[k] = acc[0] * inv; down[k + 1] = acc[1] * inv; down[k + 2] = acc[2] * inv
    res = [s_ - d for s_, d in zip(src, down)]
    big = bilinear_signed(res, w, h, 3, n)
    return bytes(min(255, max(0, int(u + r + 0.5))) for u, r in zip(up, big))


def mix(a, b, f):
    """(1-f)*a + f*b, bytewise."""
    k = int(round(f * 256))
    return bytes((x * (256 - k) + z * k + 128) >> 8 for x, z in zip(a, b))


def crop(px, w, x0, y0, cw, ch_, ch):
    return b"".join(px[((y0 + y) * w + x0) * ch:((y0 + y) * w + x0 + cw) * ch] for y in range(ch_))


def keyed_rgba(rgb, alpha, n):
    """RGBA from RGB and a 0/255 alpha, the colour 0 where alpha is 0."""
    rgba = bytearray(4 * n)
    rgba[0::4] = rgb[0::3]; rgba[1::4] = rgb[1::3]; rgba[2::4] = rgb[2::3]
    m4 = bytearray(4 * n)
    m4[0::4] = alpha; m4[1::4] = alpha; m4[2::4] = alpha; m4[3::4] = b"\xff" * n
    v = int.from_bytes(rgba, "little") & int.from_bytes(m4, "little")
    rgba = bytearray(v.to_bytes(4 * n, "little"))
    rgba[3::4] = alpha
    return bytes(rgba)


# ---- the commands ------------------------------------------------------------
def cmd_export(out, match):
    out = safe_out(out)
    for d in ("src", "in"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    tex = shipped(match)
    index = {}
    for k, e in tex.items():
        w, h, rgb = e["w"], e["h"], e["rgb"]
        keyed = key_mask(rgb, w * h)
        nkey = sum(keyed)
        write_png(os.path.join(out, "src", k + ".png"), w, h, rgb, 3)
        fill = dilate(rgb, keyed, w, h) if 0 < nkey < w * h else rgb
        p, pw, ph = padded(fill, w, h, 3)
        write_png(os.path.join(out, "in", k + ".png"), pw, ph, p, 3)
        if nkey:
            mask = bytes(0 if kk else 255 for kk in keyed for _ in range(3))
            mp, _, _ = padded(mask, w, h, 3)
            write_png(os.path.join(out, "in", k + "_mask.png"), pw, ph, mp, 3)
        index[k] = {kk: e[kk] for kk in ("name", "model", "material", "w", "h")}
        index[k]["keyed"] = nkey
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump({"pad": PAD, "textures": index}, f, indent=1, sort_keys=True)
    print(f"exported {len(index)} textures ({sum(1 for v in index.values() if v['keyed'])} keyed) "
          f"to {out}")


def cmd_upscale(out, binary, model, tile):
    out = safe_out(out)
    os.makedirs(os.path.join(out, "up"), exist_ok=True)
    cmd = [binary, "-i", os.path.join(out, "in"), "-o", os.path.join(out, "up"),
           "-n", model, "-s", "4", "-f", "png"]
    if tile:
        cmd += ["-t", str(tile)]
    mdir = os.path.join(os.path.dirname(os.path.abspath(binary)), "models")
    if os.path.isdir(mdir):
        cmd += ["-m", mdir]
    print(" ".join(cmd))
    sys.exit(subprocess.run(cmd).returncode)


def cmd_build(out, nearest_n, solid, match, grain=0.0, anchored=True):
    out = safe_out(out)
    idx = json.load(open(os.path.join(out, "index.json")))
    pad = idx["pad"]
    pack = os.path.join(out, "pack")
    os.makedirs(pack, exist_ok=True)
    need_src = nearest_n or solid
    tex = shipped(match) if need_src else None
    made = skipped = 0
    for k, e in sorted(idx["textures"].items()):
        if match and not any(s.lower() in e["model"].lower() for s in match):
            continue
        w, h = e["w"], e["h"]
        if need_src:
            if k not in tex:
                skipped += 1
                continue
            rgb = tex[k]["rgb"]
            keyed = key_mask(rgb, w * h)
            n = nearest_n or solid[1]
            alpha = nearest(bytes(0 if kk else 255 for kk in keyed), w, h, 1, n)
            col = nearest(rgb, w, h, 3, n) if nearest_n else bytes(solid[0]) * (w * n * h * n)
            write_png(os.path.join(pack, k + ".png"), w * n, h * n,
                      keyed_rgba(col, alpha, w * n * h * n), 4)
            made += 1
            continue
        up = os.path.join(out, "up", k + ".png")
        if not os.path.exists(up):
            skipped += 1
            continue
        uw, uh, rgba = read_png(up)
        s = uw // (w + 2 * pad)
        if s < 1 or uw != s * (w + 2 * pad) or uh != s * (h + 2 * pad) or s & (s - 1):
            print(f"{k}: {uw}x{uh} is not a power-of-two multiple of {w + 2 * pad}x{h + 2 * pad} - skipped")
            skipped += 1
            continue
        c = crop(rgba, uw, pad * s, pad * s, w * s, h * s, 4)
        rgb = bytearray(3 * w * s * h * s)
        rgb[0::3] = c[0::4]; rgb[1::3] = c[1::4]; rgb[2::3] = c[2::4]
        n = w * s * h * s
        if anchored or grain > 0:
            src = tex3dt.textures(os.path.join(omkpaths.data_root(), e["model"]))[e["material"]]["rgb"]
        if anchored:
            rgb = bytearray(anchor(bytes(rgb), src, w, h, s))
        if grain > 0:
            # THE GRAIN KEPT: an AI upscaler denoises, and the painted grain
            # of a floor or a ceiling is part of the art - mix a plain
            # bilinear enlargement of the original back in
            rgb = bytearray(mix(bytes(rgb), bilinear(src, w, h, 3, s), grain))
        if e["keyed"]:
            mup = os.path.join(out, "up", k + "_mask.png")
            if os.path.exists(mup):
                mw, mh, mr = read_png(mup)
                mc = crop(mr, mw, pad * s, pad * s, w * s, h * s, 4)
                alpha = mc[0::4].translate(bytes(255 if v >= 128 else 0 for v in range(256)))
            else:
                src = tex3dt.textures(os.path.join(omkpaths.data_root(), e["model"]))[e["material"]]["rgb"]
                alpha = nearest(bytes(0 if kk else 255 for kk in key_mask(src, w * h)), w, h, 1, s)
        else:
            alpha = b"\xff" * n
        write_png(os.path.join(pack, k + ".png"), w * s, h * s, keyed_rgba(bytes(rgb), alpha, n), 4)
        made += 1
    print(f"built {made} replacement textures into {pack} ({skipped} without an upscaled image)")


def main(argv):
    argv = omkpaths.take_flags(list(argv))
    if len(argv) < 3 or argv[1] not in ("export", "upscale", "build"):
        sys.exit(__doc__)
    cmd, out, rest = argv[1], argv[2], argv[3:]
    match, binary, model, tile, nearest_n, solid, grain, anchored = [], None, "realesrgan-x4plus", 0, 0, None, 0.0, True
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == "--match": match.append(rest[i + 1]); i += 2
        elif a == "--bin": binary = rest[i + 1]; i += 2
        elif a == "--model": model = rest[i + 1]; i += 2
        elif a == "--tile": tile = int(rest[i + 1]); i += 2
        elif a == "--nearest": nearest_n = int(rest[i + 1]); i += 2
        elif a == "--grain": grain = float(rest[i + 1]); i += 2
        elif a == "--no-anchor": anchored = False; i += 1
        elif a == "--solid":
            solid = (tuple(int(v) for v in rest[i + 1].split(",")), int(rest[i + 2])); i += 3
        else: sys.exit(f"unknown argument {a}\n\n{__doc__}")
    for n in (nearest_n, solid[1] if solid else 0):
        if n and (n < 1 or n > 16 or n & (n - 1)):
            sys.exit("the factor must be a power of two, 1..16")
    if cmd == "export":
        cmd_export(out, match)
    elif cmd == "upscale":
        if not binary:
            sys.exit("upscale needs --bin PATH (realesrgan-ncnn-vulkan)")
        cmd_upscale(out, binary, model, tile)
    else:
        cmd_build(out, nearest_n, solid, match, grain, anchored)


if __name__ == "__main__":
    main(sys.argv)
