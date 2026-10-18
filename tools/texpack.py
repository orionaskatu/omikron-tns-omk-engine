#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""TEXTURE OVERRIDE PACKS - `todo/enhancements.md` 25, `[Enhancements] texturepack`.

The game's model and set textures are small palettised images (most 256x256,
8 bits a texel) in the `.3DT` beside each `.3DO`. A pack replaces any of them,
for the Vulkan and Direct3D 12 renderers, with a PNG of the same picture at an
INTEGER multiple of its size (1x, 2x, 4x, ...) - without touching the game data.

    python3 tools/texpack.py extract  OUT [--group G] [--model NAME]
        every texture to OUT/<group>/<model>/<MATERIAL>.png (group as on disk:
        DECORS, MISC, OBJETS, PERSOS); --model/--group narrow it (fnmatch,
        case-insensitive)
    python3 tools/texpack.py upscale  IN OUT --factor 2|4 [--tool EXE]
            [--model realesrgan-x4plus] [--method lanczos|nearest] [--jobs N]
        every PNG under IN to OUT, same tree. With --tool, that external
        upscaler runs once per file (Real-ESRGAN-ncnn-vulkan's command line:
        EXE -i in.png -o out.png -n MODEL -s FACTOR -f png, run from EXE's own
        directory so it finds its models/; realesrgan-x4plus is a 4x model - use
        --factor 4 with it, or a model that takes -s 2); without it the built-in
        Lanczos-3 + mild unsharp mask (pure Python - slow, a baseline only)
    python3 tools/texpack.py check    PACK
        what the pack overrides, model by model, and what the engine will reject

    and in omk-remaster.ini:   [Enhancements]
                               texturepack = C:/path/to/OUT   (relative: to the ini)
    or on the command line:    omk-play ... --texturepack OUT

THE COLOUR KEY. A texel that is pure black (0,0,0) is the engine's colour key:
a cutout batch (a grille, a leaf, a sign's outline) does not draw it, any other
batch draws it black. A pack keeps that rule - the engine keys every pure-black
texel of a replacement, and an RGBA PNG's alpha < 128 too. `upscale` therefore
treats the key apart: the key texels are filled with their neighbours' colour
before the picture is upscaled (so no black halo bleeds into the edge), and
after it the key is put back from the ORIGINAL's mask (bilinear, thresholded at
one half) as pure black, while a non-key texel the upscaler happened to make
pure black is lifted to (1,1,1) so it does not become a hole.

THE NAME. `<MATERIAL>` is the material record's own name (+0, the `.BMP` file
name at +20 without its extension), any character outside `A-Za-z0-9 _+.~-`
(and a trailing space or dot) as `_`; a name a model repeats (case aside - one
model has `Door CS` and `DOOR CS`) takes `~<material index>` on the later one.
The engine applies the same rule (`o3de/texpack.cpp` `packFileName`). It then
matches a replacement to what it draws by the original's PIXELS (indices and
palette), so a texture two models share takes whichever replacement is found
first, and a texture the game's slot cache swaps in from another model (the
Anekbah sign, `o3de/texcache.h`) takes that model's replacement.

Pure stdlib (zlib for PNG). Never writes into the game data.
"""
import fnmatch, os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import omkpaths, mesh3do, tex3dt  # noqa: E402

SAFE = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _+.~-")


# ---- names -------------------------------------------------------------------

def material_names(do_bytes):
    """The +0 name of every material, in material order."""
    h = mesh3do.header_bytes(do_bytes, len(do_bytes))
    out = []
    for i in range(h["materials"]):
        o = h["matOff"] + 80 * i
        out.append(do_bytes[o:o + 20].split(b"\0")[0].decode("cp1252", "replace"))
    return out


def pack_file_names(names):
    """-> the file stem a pack uses for each material (see the module notes)."""
    out, seen = [], set()
    for i, n in enumerate(names):
        s = "".join(c if c in SAFE else "_" for c in n)
        k = len(s)
        while k > 0 and s[k - 1] in " .":
            k -= 1
        s = s[:k] + "_" * (len(s) - k)
        if not s:
            s = "mat%d" % i
        if s.lower() in seen:
            s = "%s~%d" % (s, i)
        seen.add(s.lower())
        out.append(s)
    return out


def models(root, group_pat="*", model_pat="*"):
    """-> [(group, model stem, .3DO path)] under MESHES, sorted."""
    meshes = None
    for n in os.listdir(root):
        if n.lower() == "meshes":
            meshes = os.path.join(root, n)
    if not meshes:
        sys.exit("no MESHES directory under %s" % root)
    out = []
    for g in sorted(os.listdir(meshes)):
        gd = os.path.join(meshes, g)
        if not os.path.isdir(gd) or not fnmatch.fnmatch(g.lower(), group_pat.lower()):
            continue
        for f in sorted(os.listdir(gd)):
            stem, ext = os.path.splitext(f)
            if ext.lower() == ".3do" and fnmatch.fnmatch(stem.lower(), model_pat.lower()):
                out.append((g, stem, os.path.join(gd, f)))
    return out


def guard_output(path):
    """Refuse an output inside the game data (CLAUDE.md §1, `safeOutputPath`)."""
    root = os.path.normcase(os.path.realpath(omkpaths.data_root()))
    p = os.path.normcase(os.path.realpath(path))
    if p == root or p.startswith(root + os.sep):
        sys.exit("refusing to write inside the game data: %s" % path)


# ---- PNG ---------------------------------------------------------------------

def png_write(path, w, h, rgb):
    with open(path, "wb") as f:
        f.write(tex3dt.png(w, h, bytes(rgb)))


def png_read(path):
    """-> (w, h, channels, bytes) for 8-bit grey/RGB/palette/grey-alpha/RGBA,
    non-interlaced; channels 3 or 4 (grey and palette are expanded)."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    o, idat, plte, trns = 8, [], None, None
    w = h = depth = ctype = inter = None
    while o + 8 <= len(d):
        n, tag = struct.unpack_from(">I4s", d, o)
        body = d[o + 8:o + 8 + n]
        o += 12 + n
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    if depth != 8 or inter != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError("unsupported PNG (bit depth %s, colour type %s, interlace %s)"
                         % (depth, ctype, inter))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif ft != 0:
            raise ValueError("bad PNG filter %d" % ft)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if ctype == 2 or ctype == 6:
        return w, h, bpp, bytes(out)
    if ctype == 0:
        return w, h, 3, bytes(b for v in out for b in (v, v, v))
    if ctype == 4:
        return w, h, 4, bytes(b for k in range(0, len(out), 2)
                              for b in (out[k], out[k], out[k], out[k + 1]))
    rgba = bytearray()
    for v in out:
        a = trns[v] if trns and v < len(trns) else 255
        rgba += plte[3 * v:3 * v + 3] + bytes([a])
    return w, h, 4, bytes(rgba)


def keyed_rgb(w, h, ch, px):
    """-> (rgb, key mask) by the ENGINE's rule: pure black, or alpha < 128."""
    n = w * h
    rgb = bytearray(3 * n)
    key = bytearray(n)
    for k in range(n):
        r, g, b = px[ch * k], px[ch * k + 1], px[ch * k + 2]
        if (r | g | b) == 0 or (ch == 4 and px[ch * k + 3] < 128):
            key[k] = 1
        else:
            rgb[3 * k:3 * k + 3] = bytes((r, g, b))
    return rgb, key


# ---- extract -----------------------------------------------------------------

def cmd_extract(argv):
    gp, mp = opt(argv, "--group", "*"), opt(argv, "--model", "*")
    out = argv[0] if argv else sys.exit("extract OUT [--group G] [--model NAME]")
    guard_output(out)
    nt = nm = 0
    for g, stem, path in models(omkpaths.data_root(), gp, mp):
        tex = tex3dt.textures(path)
        if not tex:
            continue
        names = pack_file_names(material_names(open(path, "rb").read()))
        dd = os.path.join(out, g, stem)
        os.makedirs(dd, exist_ok=True)
        for t, fn in zip(tex, names):
            if t["w"] <= 0 or t["h"] <= 0:
                continue
            png_write(os.path.join(dd, fn + ".png"), t["w"], t["h"], t["rgb"])
            nt += 1
        nm += 1
    print("extract: %d textures of %d models -> %s" % (nt, nm, out))


# ---- upscale -----------------------------------------------------------------

def dilate_key(w, h, rgb, key):
    """Fill every key texel with the mean of its non-key neighbours, ring by
    ring, so an upscaler sees colour, not black, across a cutout's edge."""
    rgb = bytearray(rgb)
    known = [not k for k in key]
    if not any(known):
        return rgb
    todo = [k for k in range(w * h) if not known[k]]
    while todo:
        fill, rest = [], []
        for k in todo:
            x, y = k % w, k // w
            s = [0, 0, 0]; c = 0
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                xx, yy = (x + dx) % w, (y + dy) % h       # the sampler WRAPS
                j = yy * w + xx
                if known[j]:
                    s[0] += rgb[3 * j]; s[1] += rgb[3 * j + 1]; s[2] += rgb[3 * j + 2]; c += 1
            if c:
                fill.append((k, s[0] // c, s[1] // c, s[2] // c))
            else:
                rest.append(k)
        if not fill:
            break
        for k, r, g, b in fill:
            rgb[3 * k:3 * k + 3] = bytes((r, g, b)); known[k] = True
        todo = rest
    return rgb


def lanczos_weights(n_in, n_out, a=3):
    """Per output coordinate: [(input index, weight)], the texture WRAPPING."""
    import math
    def L(x):
        if x == 0:
            return 1.0
        if -a < x < a:
            px = math.pi * x
            return a * math.sin(px) * math.sin(px / a) / (px * px)
        return 0.0
    s = n_in / n_out
    out = []
    for j in range(n_out):
        c = (j + 0.5) * s - 0.5
        lo = math.floor(c) - a + 1
        taps = [(i % n_in, L(c - i)) for i in range(lo, lo + 2 * a)]
        t = sum(wt for _, wt in taps)
        out.append([(i, wt / t) for i, wt in taps if wt])
    return out


def resample(w, h, chan, W, H):
    """One channel (a list of floats, w*h) to W*H, separable Lanczos-3, wrapping."""
    wx, wy = lanczos_weights(w, W), lanczos_weights(h, H)
    tmp = [0.0] * (W * h)
    for y in range(h):
        row = chan[y * w:(y + 1) * w]
        base = y * W
        for x, taps in enumerate(wx):
            tmp[base + x] = sum(row[i] * wt for i, wt in taps)
    out = [0.0] * (W * H)
    for y, taps in enumerate(wy):
        base = y * W
        rows = [(i * W, wt) for i, wt in taps]
        for x in range(W):
            out[base + x] = sum(tmp[r + x] * wt for r, wt in rows)
    return out


def unsharp(W, H, chan, amount=0.45):
    """chan + amount * (chan - blur), the blur a wrapping [1 2 1]/4 each way."""
    tmp = [0.0] * (W * H)
    for y in range(H):
        b = y * W
        for x in range(W):
            tmp[b + x] = (chan[b + (x - 1) % W] + 2 * chan[b + x] + chan[b + (x + 1) % W]) * 0.25
    out = [0.0] * (W * H)
    for y in range(H):
        b, u, d = y * W, ((y - 1) % H) * W, ((y + 1) % H) * W
        for x in range(W):
            bl = (tmp[u + x] + 2 * tmp[b + x] + tmp[d + x]) * 0.25
            out[b + x] = chan[b + x] + amount * (chan[b + x] - bl)
    return out


def upscale_mask(w, h, key, W, H):
    """The original's key at W x H: bilinear over the 0/1 mask, >= 0.5 is key."""
    out = bytearray(W * H)
    sx, sy = w / W, h / H
    for y in range(H):
        fy = (y + 0.5) * sy - 0.5
        y0 = int(fy // 1); ty = fy - y0
        r0, r1 = (y0 % h) * w, ((y0 + 1) % h) * w
        for x in range(W):
            fx = (x + 0.5) * sx - 0.5
            x0 = int(fx // 1); tx = fx - x0
            a, b = x0 % w, (x0 + 1) % w
            v = ((key[r0 + a] * (1 - tx) + key[r0 + b] * tx) * (1 - ty)
                 + (key[r1 + a] * (1 - tx) + key[r1 + b] * tx) * ty)
            out[y * W + x] = 1 if v >= 0.5 else 0
    return out


def rekey(W, H, rgb, mask):
    """Key texels pure black; a non-key texel that came out pure black -> (1,1,1)."""
    rgb = bytearray(rgb)
    for k in range(W * H):
        if mask[k]:
            rgb[3 * k:3 * k + 3] = b"\0\0\0"
        elif rgb[3 * k] == 0 and rgb[3 * k + 1] == 0 and rgb[3 * k + 2] == 0:
            rgb[3 * k:3 * k + 3] = b"\1\1\1"
    return rgb


def upscale_builtin(w, h, rgb, f, method):
    W, H = w * f, h * f
    if method == "nearest":
        out = bytearray(3 * W * H)
        for y in range(H):
            sr = (y // f) * w
            for x in range(W):
                s = 3 * (sr + x // f)
                out[3 * (y * W + x):3 * (y * W + x) + 3] = rgb[s:s + 3]
        return W, H, out
    chans = []
    for c in range(3):
        ch = resample(w, h, [float(v) for v in rgb[c::3]], W, H)
        chans.append(unsharp(W, H, ch))
    out = bytearray(3 * W * H)
    for c in range(3):
        out[c::3] = bytes(0 if v < 0 else 255 if v > 255 else int(v + 0.5) for v in chans[c])
    return W, H, out


def upscale_one(job):
    src, dst, f, tool, model, method = job
    try:
        w, h, ch, px = png_read(src)
        rgb, key = keyed_rgb(w, h, ch, px)
        if method == "nearest" and not tool:
            # exact replication: the key travels with the texels, nothing to fix
            W, H, out = upscale_builtin(w, h, rgb, f, "nearest")
            png_write(dst, W, H, out)
            return src, "%dx%d -> %dx%d" % (w, h, W, H)
        filled = dilate_key(w, h, rgb, key) if any(key) else rgb
        if tool:
            with tempfile.TemporaryDirectory() as tmp:
                a, b = os.path.join(tmp, "in.png"), os.path.join(tmp, "out.png")
                png_write(a, w, h, filled)
                # from the tool's own directory, where Real-ESRGAN-ncnn-vulkan
                # looks for its `models/`
                r = subprocess.run([os.path.abspath(tool), "-i", a, "-o", b, "-n", model, "-s", str(f),
                                    "-f", "png"], capture_output=True, text=True,
                                   cwd=os.path.dirname(os.path.abspath(tool)))
                if r.returncode != 0 or not os.path.exists(b):
                    return src, "FAILED: %s exited %d %s" % (tool, r.returncode, r.stderr.strip()[-200:])
                W, H, ch2, px2 = png_read(b)
            if W % w or H % h or W // w != H // h:
                return src, "FAILED: the tool made %dx%d of %dx%d - not an integer multiple" % (W, H, w, h)
            out = bytes(px2) if ch2 == 3 else bytes(v for k in range(W * H) for v in px2[4 * k:4 * k + 3])
        else:
            W, H, out = upscale_builtin(w, h, filled, f, method)
        mask = upscale_mask(w, h, key, W, H) if any(key) else bytearray(W * H)
        png_write(dst, W, H, rekey(W, H, out, mask))
        return src, "%dx%d -> %dx%d" % (w, h, W, H)
    except Exception as e:  # one bad file must not stop a pack
        return src, "FAILED: %s" % e


def cmd_upscale(argv):
    f = int(opt(argv, "--factor", "2"))
    tool = opt(argv, "--tool", None)
    model = opt(argv, "--model", "realesrgan-x4plus")
    method = opt(argv, "--method", "lanczos")
    jobs = int(opt(argv, "--jobs", str(os.cpu_count() or 1)))
    if len(argv) < 2:
        sys.exit("upscale IN OUT --factor 2|4 [--tool EXE] [--model M] [--method lanczos|nearest] [--jobs N]")
    src, dst = argv[0], argv[1]
    if f < 1 or f > 16:
        sys.exit("--factor: 1..16")
    if tool and not os.path.exists(tool):
        sys.exit("--tool %s: no such file" % tool)
    guard_output(dst)
    work = []
    for d, _, files in os.walk(src):
        for fn in sorted(files):
            if fn.lower().endswith(".png"):
                rel = os.path.relpath(os.path.join(d, fn), src)
                os.makedirs(os.path.join(dst, os.path.dirname(rel)), exist_ok=True)
                work.append((os.path.join(d, fn), os.path.join(dst, rel), f, tool, model, method))
    bad = 0
    if jobs > 1 and len(work) > 1 and not tool:
        import multiprocessing
        with multiprocessing.Pool(jobs) as p:
            results = p.imap(upscale_one, work)
            for s, msg in results:
                bad += msg.startswith("FAILED")
                print("  %s: %s" % (os.path.relpath(s, src), msg), flush=True)
    else:
        for job in work:
            s, msg = upscale_one(job)
            bad += msg.startswith("FAILED")
            print("  %s: %s" % (os.path.relpath(s, src), msg), flush=True)
    print("upscale: %d files, %d failed, x%d %s -> %s"
          % (len(work), bad, f, ("with " + tool) if tool else "built-in " + method, dst))
    return 1 if bad else 0


# ---- check -------------------------------------------------------------------

def png_header(path):
    with open(path, "rb") as fh:
        d = fh.read(33)
    if len(d) < 33 or d[:8] != b"\x89PNG\r\n\x1a\n" or d[12:16] != b"IHDR":
        return None
    return struct.unpack(">IIBBBBB", d[16:29])


def cmd_check(argv):
    pack = argv[0] if argv else sys.exit("check PACK")
    root = omkpaths.data_root()
    byname = {(g.lower(), s.lower()): (g, s, p) for g, s, p in models(root)}
    ok = bad = stray = 0
    scales = {}
    for g in sorted(os.listdir(pack)):
        gd = os.path.join(pack, g)
        if not os.path.isdir(gd):
            continue
        for m in sorted(os.listdir(gd)):
            md = os.path.join(gd, m)
            if not os.path.isdir(md):
                continue
            hit = byname.get((g.lower(), m.lower()))
            files = {f.lower(): f for f in os.listdir(md) if f.lower().endswith(".png")}
            if not hit:
                print("%s/%s: no such model in the game data (%d files ignored)" % (g, m, len(files)))
                stray += len(files)
                continue
            path = hit[2]
            tex = tex3dt.textures(path)
            names = pack_file_names(material_names(open(path, "rb").read()))
            lines, used = [], set()
            for t, fn in zip(tex, names):
                f = files.get(fn.lower() + ".png")
                if not f:
                    continue
                used.add(fn.lower() + ".png")
                hd = png_header(os.path.join(md, f))
                why = None
                if not hd:
                    why = "not a PNG"
                else:
                    W, H, depth, ctype, _, _, inter = hd
                    if depth != 8 or ctype not in (2, 6) or inter:
                        why = "PNG %d-bit colour type %d%s: the engine reads 8-bit RGB/RGBA, " \
                              "non-interlaced" % (depth, ctype, " interlaced" if inter else "")
                    elif W % t["w"] or H % t["h"] or W // t["w"] != H // t["h"]:
                        why = "%dx%d is not an integer multiple of %dx%d" % (W, H, t["w"], t["h"])
                if why:
                    bad += 1
                    lines.append("   %-22s REJECTED: %s" % (f, why))
                else:
                    ok += 1
                    k = W // t["w"]
                    scales[k] = scales.get(k, 0) + 1
                    lines.append("   %-22s %dx%d -> %dx%d (x%d)" % (f, t["w"], t["h"], W, H, k))
            for f in sorted(set(files) - used):
                stray += 1
                lines.append("   %-22s no material of this model has that name" % files[f])
            print("%s/%s: %d of %d textures overridden" % (hit[0], hit[1], len(used), len(tex)))
            for l in lines:
                print(l)
    print("check: %d overrides%s, %d rejected, %d files matching nothing"
          % (ok, (" (" + ", ".join("x%d %d" % (k, v) for k, v in sorted(scales.items())) + ")")
              if scales else "", bad, stray))
    return 1 if bad else 0


def opt(argv, name, default):
    if name in argv:
        i = argv.index(name)
        if i + 1 < len(argv):
            v = argv[i + 1]
            del argv[i:i + 2]
            return v
        sys.exit("%s needs a value" % name)
    return default


def main():
    argv = omkpaths.take_flags(sys.argv)[1:]
    if not argv or argv[0] not in ("extract", "upscale", "check"):
        print(__doc__.split("THE COLOUR KEY")[0])
        print(omkpaths.FLAG_HELP)
        return 2
    cmd, rest = argv[0], argv[1:]
    return {"extract": cmd_extract, "upscale": cmd_upscale, "check": cmd_check}[cmd](rest) or 0


if __name__ == "__main__":
    sys.exit(main())
