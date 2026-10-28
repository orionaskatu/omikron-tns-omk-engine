#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the shader headers AMD's FidelityFX SDK v1.1.4 needs for FSR 3.1, on Linux.

The SDK compiles its GLSL with `FidelityFX_SC.exe`, a Windows-only tool (wide
characters, <Windows.h>, a bundled glslangValidator.exe), which writes, for each
pass and each combination of the shader options, a header with the SPIR-V and
what the SDK needs to bind it (the resource names, bindings, sets), plus a
`*_permutations.h` that picks one by a key. This script does the same job for
the FSR 3.1 upscaler passes with a Linux glslangValidator, in the format
`ffx_sc.cpp` / `glsl_compiler.cpp` write (read in the SDK's
`sdk/tools/ffx_shader_compiler/src`). The SPIR-V is read directly for the
resources (what SPIRV-Reflect gives the original tool).

    gen_shaders.py --sdk <FidelityFX-SDK v1.1.4> --out <dir> [--glslang glslangValidator]
"""
import argparse
import concurrent.futures
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

# `sdk/include/FidelityFX/gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt`
# and `sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt` (checked below)
OPTIONS = [
    "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
    "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
]
BASE_DEFS = [
    "-DFFX_GPU=1",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
]
API_ARGS = ["-e", "CS", "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1"]

# the four spellings the SDK asks for: (name suffix, FFX_HALF)
VARIANTS = [("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)]


def spirv_resources(blob):
    """The descriptor bindings of a SPIR-V module, in SPIRV-Reflect's order:
    by set, then binding. Returns {kind: [(name, binding, count, set)]}."""
    w = struct.unpack("<%dI" % (len(blob) // 4), blob)
    names, deco, types, consts, vars_ = {}, {}, {}, {}, []
    i = 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        a = w[i + 1:i + n]
        if op == 5:   # OpName
            raw = struct.pack("<%dI" % (len(a) - 1), *a[1:])
            names[a[0]] = raw.split(b"\0")[0].decode()
        elif op == 71:   # OpDecorate
            deco.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else 1
        elif op == 43:   # OpConstant
            consts[a[1]] = a[2]
        elif op in (25, 26, 27, 28, 29, 30, 32):
            types[a[0]] = (op, a[1:])
        elif op == 59:   # OpVariable
            vars_.append((a[0], a[1], a[2]))
        i += n
    out = {k: [] for k in ("cbv", "srvTex", "uavTex", "srvBuf", "uavBuf", "sampler", "rt")}
    for tid, vid, sclass in vars_:
        d = deco.get(vid, {})
        if 33 not in d or 34 not in d:   # Binding, DescriptorSet
            continue
        t = types[tid][1][1]   # the pointee
        count = 1
        while types[t][0] in (28, 29):   # arrays
            if types[t][0] == 28:
                count *= consts[types[t][1][1]]
            t = types[t][1][0]
        op, args = types[t]
        name = names.get(vid, "")
        if op == 30:   # struct
            td = deco.get(t, {})
            if not name:
                name = names.get(t, "")
            if 2 in td and sclass == 2:   # Block in Uniform: a constant buffer
                kind = "cbv"
            else:                         # a storage buffer (FSR 3.1's passes have none)
                kind = "srvBuf" if 24 in d else "uavBuf"   # NonWritable
        elif op == 25:   # image
            kind = "srvTex" if args[5] == 1 else "uavTex"
        elif op == 26:
            kind = "sampler"
        else:
            raise SystemExit("unsupported descriptor type for %s" % name)
        out[kind].append((name, d[33], count, d[34]))
    for k in out:
        out[k].sort(key=lambda r: (r[3], r[1]))
    return out


def includes_text(path, dirs, seen):
    """The shader's text with every file it includes, for finding the options."""
    if path in seen:
        return ""
    seen.add(path)
    with open(path, "r", errors="replace") as f:
        text = f.read()
    out = text
    for m in re.finditer(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M):
        for d in [os.path.dirname(path)] + dirs:
            p = os.path.join(d, m.group(1))
            if os.path.isfile(p):
                out += includes_text(p, dirs, seen)
                break
    return out


def compile_one(glslang, shader, incs, half, defs, tmp):
    spv = os.path.join(tmp, "p%d_%s.spv" % (half, "_".join(d.split("=")[-1] for d in defs) or "x"))
    cmd = [glslang] + defs + BASE_DEFS + API_ARGS + ["-DFFX_HALF=%d" % half] \
        + ["-I" + d for d in incs] + ["-o", spv, shader]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
        raise SystemExit("glslang failed for " + shader)
    with open(spv, "rb") as f:
        return f.read()


def res_decl(pname, kind, rows):
    cname = {"cbv": "CBV", "srvTex": "TextureSRV", "uavTex": "TextureUAV", "srvBuf": "BufferSRV",
             "uavBuf": "BufferUAV", "sampler": "Sampler", "rt": "RTAccelerationStructure"}[kind]
    if not rows:
        return ""
    s = "static const char* g_%s_%sResourceNames[] = { " % (pname, cname)
    s += "".join(' "%s",' % r[0] for r in rows) + " };\n"
    for label, idx in (("Bindings", 1), ("Counts", 2), ("Sets", 3)):
        s += "static const uint32_t g_%s_%sResource%s[] = { " % (pname, cname, label)
        s += "".join(" %d," % r[idx] for r in rows) + " };\n"
    return s + "\n"


KINDS = ["cbv", "srvTex", "uavTex", "srvBuf", "uavBuf", "sampler", "rt"]
CNAMES = {"cbv": "CBV", "srvTex": "TextureSRV", "uavTex": "TextureUAV", "srvBuf": "BufferSRV",
          "uavBuf": "BufferUAV", "sampler": "Sampler", "rt": "RTAccelerationStructure"}
STRUCT_MEMBERS = [
    ("numConstantBuffers", "constantBuffer"), ("numSRVTextures", "srvTexture"),
    ("numUAVTextures", "uavTexture"), ("numSRVBuffers", "srvBuffer"),
    ("numUAVBuffers", "uavBuffer"), ("numSamplers", "sampler"),
    ("numRTAccelerationStructures", "rtAccelerationStructure"),
]


def write_blob_header(outdir, name, digest, blob, res):
    pname = "%s_%s" % (name, digest)
    s = "// %s.h.\n// Auto generated by gen_shaders.py (after FidelityFX-SC).\n\n" % pname
    for k in KINDS:
        s += res_decl(pname, k, res[k])
    s += "static const uint32_t g_%s_size = %d;\n\n" % (pname, len(blob))
    s += "static const unsigned char g_%s_data[] = {\n" % pname
    n = len(blob)
    s += "".join("0x%02x%s" % (b, "" if i == n - 1 else (",\n" if (i + 1) % 16 == 0 else ","))
                 for i, b in enumerate(blob))
    s += "\n};\n\n"
    with open(os.path.join(outdir, pname + ".h"), "w", newline="\n") as f:
        f.write(s)


def write_permutations(outdir, name, order, key_to_index):
    s = "".join('#include "%s_%s.h"\n' % (name, d) for d, _, _ in order) + "\n"
    union = name + "_PermutationKey"
    s += "typedef union %s {\n    struct {\n" % union
    for o in KEY_ORDER:
        s += "        uint32_t %s : 1;\n" % o
    s += "    };\n    uint32_t index;\n} %s;\n\n" % union
    s += "typedef struct %s_PermutationInfo {\n    const uint32_t       blobSize;\n" % name
    s += "    const unsigned char* blobData;\n\n"
    for nm, base in STRUCT_MEMBERS:
        s += "    const uint32_t  %s;\n    const char**    %sNames;\n" % (nm, base)
        s += "    const uint32_t* %sBindings;\n    const uint32_t* %sCounts;\n" % (base, base)
        s += "    const uint32_t* %sSpaces;\n\n" % base
    s = s.rstrip("\n") + "\n} %s_PermutationInfo;\n\n" % name
    s += "static const uint32_t g_%s_IndirectionTable[] = {\n" % name
    s += "".join("    %d,\n" % key_to_index.get(k, 0) for k in range(1 << len(KEY_ORDER)))
    s += "};\n\n"
    s += "static const %s_PermutationInfo g_%s_PermutationInfo[] = {\n" % (name, name)
    for d, blob, res in order:
        pn = "%s_%s" % (name, d)
        s += "    { g_%s_size, g_%s_data, " % (pn, pn)
        for k in KINDS:
            rows = res[k]
            if not rows:
                s += "0, 0, 0, 0, 0, "
            else:
                c = CNAMES[k]
                s += "%d, g_%s_%sResourceNames, g_%s_%sResourceBindings, g_%s_%sResourceCounts, g_%s_%sResourceSets, " % (
                    len(rows), pn, c, pn, c, pn, c, pn, c)
        s += "},\n"
    s += "};\n\n"
    with open(os.path.join(outdir, name + "_permutations.h"), "w", newline="\n") as f:
        f.write(s)


KEY_ORDER = []


def main():
    global KEY_ORDER
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--glslang", default="glslangValidator")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()

    gpu = os.path.join(a.sdk, "sdk", "include", "FidelityFX", "gpu")
    shaders_dir = os.path.join(a.sdk, "sdk", "src", "backends", "vk", "shaders", "fsr3upscaler")
    cm = open(os.path.join(gpu, "fsr3upscaler", "CMakeCompileFSR3UpscalerShaders.txt")).read()
    for o in OPTIONS + [d[2:].split("=")[0] for d in BASE_DEFS]:
        if o not in cm:
            raise SystemExit("the SDK's shader options changed: %s is not in its CMake file" % o)
    incs = [gpu, os.path.join(gpu, "fsr3upscaler")]
    os.makedirs(a.out, exist_ok=True)

    passes = sorted(f for f in os.listdir(shaders_dir) if f.endswith(".glsl"))
    pool = concurrent.futures.ThreadPoolExecutor(a.jobs)
    with tempfile.TemporaryDirectory() as tmp:
        for fn in passes:
            base = fn[:-len(".glsl")]
            shader = os.path.join(shaders_dir, fn)
            text = includes_text(shader, incs, set())
            found = [o for o in OPTIONS if o in text]
            # the original puts the options found in the shader first (stable)
            KEY_ORDER = found + [o for o in OPTIONS if o not in found]
            nfound = len(found)
            keys = range(1 << nfound)

            def defs_of(k):
                return ["-D%s=%d" % (found[b], (k >> b) & 1) for b in range(nfound)] + \
                       ["-D%s=0" % o for o in OPTIONS if o not in found]

            results = {}   # half -> key -> (digest, blob)
            for half in (0, 1):
                futs = {k: pool.submit(compile_one, a.glslang, shader, incs, half, defs_of(k), tmp) for k in keys}
                results[half] = {}
                for k, f in futs.items():
                    blob = f.result()
                    results[half][k] = (hashlib.md5(blob).hexdigest(), blob)
            for suffix, half in VARIANTS:
                name = base + suffix
                order, index_of, key_to_index = [], {}, {}
                for full_key in range(1 << len(KEY_ORDER)):
                    d, blob = results[half][full_key & ((1 << nfound) - 1)]
                    if d not in index_of:
                        index_of[d] = len(order)
                        order.append((d, blob, spirv_resources(blob)))
                    key_to_index[full_key] = index_of[d]
                for d, blob, res in order:
                    write_blob_header(a.out, name, d, blob, res)
                write_permutations(a.out, name, order, key_to_index)
            print("%s: %d options in the shader, %d + %d unique blobs" % (
                base, nfound, len({d for d, _ in results[0].values()}), len({d for d, _ in results[1].values()})))


if __name__ == "__main__":
    main()
