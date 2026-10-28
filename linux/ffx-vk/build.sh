#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds libamd_fidelityfx_vk.so - AMD FSR 3.1 over Vulkan - on Linux, from the
# FidelityFX SDK v1.1.4's source (MIT). AMD ships that library for Windows only
# (PrebuiltSignedDLL/amd_fidelityfx_vk.dll) and builds it with Visual Studio and a
# Windows-only shader tool; this is the same code, built here by:
#   * gen_shaders.py - the shader headers FidelityFX_SC.exe would write;
#   * compat.h - the few MSVC-isms (the "_s" string functions, _countof);
#   * ffx_provider_linux.cpp - the SDK's provider list without the Windows driver
#     hooks, FSR 2 and frame generation (only the FSR 3.1 upscaler is wanted).
#
#   build.sh <FidelityFX-SDK v1.1.4 folder> <output folder>
#
# Needs g++, python3, glslangValidator (Debian / Ubuntu: glslang-tools) and the
# Vulkan headers and loader (libvulkan-dev).
set -euo pipefail

sdk=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
jobs=$(nproc 2>/dev/null || echo 2)

mkdir -p "$out/shaders" "$out/include" "$out/src" "$out/obj"
# the SDK's headers, with the context made bigger: its private context holds wide
# strings, and a wchar_t is 4 bytes here (2 on Windows), so the 128K words the
# public struct has are not enough (a static assert says so). The context is
# opaque to the caller (ffx-api allocates it), so only this library sees the size.
rm -rf "$out/sdkinclude"
cp -r "$sdk/sdk/include" "$out/sdkinclude"
sed -i 's/(1024 \* 128)/(1024 * 512)/' "$out/sdkinclude/FidelityFX/host/ffx_types.h"
grep -q "(1024 \* 512)" "$out/sdkinclude/FidelityFX/host/ffx_types.h" || { echo "the SDK's context size line changed"; exit 1; }
# a few sources spell the include folder FidelityFx
ln -sfn "$out/sdkinclude/FidelityFX" "$out/include/FidelityFx"
# MSVC's 64 bit literal suffix
sed 's/ui64/ULL/' "$sdk/ffx-api/src/ffx_provider_fsr3upscale.cpp" > "$out/src/ffx_provider_fsr3upscale.cpp"

python3 "$here/gen_shaders.py" --sdk "$sdk" --out "$out/shaders" \
    --glslang "${GLSLANG:-glslangValidator}" --jobs "$jobs"

flags=(-std=c++17 -O0 -g -fPIC -fvisibility=hidden -w -include "$here/compat.h"
       -DFFX_BACKEND_VK -DFFX_FSR3UPSCALER
       -I"$out/include" -I"$out/shaders"
       -I"$sdk/ffx-api/src" -I"$sdk/ffx-api/include" -I"$out/sdkinclude"
       -I"$sdk/sdk/src/shared" -I"$sdk/sdk/src/backends/shared"
       -I"$sdk/sdk/src/components" -I"$sdk/sdk/src/backends/vk")

sources=(
    "$sdk/ffx-api/src/ffx_api.cpp"
    "$sdk/ffx-api/src/validation.cpp"
    "$sdk/ffx-api/src/backends.cpp"
    "$out/src/ffx_provider_fsr3upscale.cpp"
    "$here/ffx_provider_linux.cpp"
    "$sdk/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "$sdk/sdk/src/backends/vk/ffx_vk.cpp"
    "$sdk/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "$sdk/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "$sdk/sdk/src/shared/ffx_assert.cpp"
    "$here/ffx_message_linux.cpp"
    "$here/ffx_stubs_linux.cpp"
    "$sdk/sdk/src/shared/ffx_breadcrumbs_list.cpp"
    "$sdk/sdk/src/shared/ffx_object_management.cpp"
)

export OUT="$out"
printf '%s\n' "${sources[@]}" | xargs -P "$jobs" -I{} sh -c \
    'g++ "$@" -c "$0" -o "$OUT/obj/$(basename "$0" .cpp).o"' {} "${flags[@]}"

so="$out/libamd_fidelityfx_vk.so"
g++ -shared -o "$so" "$out"/obj/*.o -lvulkan -ldl -lpthread \
    -Wl,-soname,libamd_fidelityfx_vk.so -Wl,--no-undefined

echo "--- exported symbols"
nm -D --defined-only "$so" | grep -E " T ffx" || true
for s in ffxCreateContext ffxDestroyContext ffxConfigure ffxQuery ffxDispatch; do
    nm -D --defined-only "$so" | grep -q " T $s\$" || { echo "MISSING export: $s"; exit 1; }
done
ls -la "$so"
