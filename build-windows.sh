#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the game (engine/build/omk-play.exe) on Windows.
#
# Run it from an MSYS2 "MINGW64" shell:   ./build-windows.sh
# Everything it needs is listed in the README ("Build it on Windows").
#
# VULKAN_SDK  where the LunarG Vulkan SDK is (found by itself in C:/VulkanSDK if not set)
# SDL2_DIR    only if SDL2 is not installed through MSYS2: the folder that holds
#             lib/pkgconfig/sdl2.pc (the "x86_64-w64-mingw32" folder of the SDL2 download)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

# --- the Vulkan SDK: it gives us the headers, vulkan-1.lib and the shader tools
if [ -z "$VULKAN_SDK" ]; then
    VULKAN_SDK="$(ls -d /c/VulkanSDK/*/ 2>/dev/null | sort | tail -n 1)"
    VULKAN_SDK="${VULKAN_SDK%/}"
fi
if [ -z "$VULKAN_SDK" ] || [ ! -d "$VULKAN_SDK" ]; then
    echo "Vulkan SDK not found. Install it from https://vulkan.lunarg.com/sdk/home#windows"
    echo "(or set VULKAN_SDK to its folder)."
    exit 1
fi
if command -v cygpath >/dev/null 2>&1; then
    VK_WIN="$(cygpath -m "$VULKAN_SDK")"
    VK_BIN="$(cygpath -u "$VULKAN_SDK")/Bin"
else
    VK_WIN="$VULKAN_SDK"
    VK_BIN="$VULKAN_SDK/Bin"
fi
for tool in glslc spirv-cross dxc; do
    if [ ! -f "$VK_BIN/$tool.exe" ]; then
        echo "$tool.exe is missing from $VK_BIN - the Vulkan SDK install looks incomplete."
        exit 1
    fi
done

# --- tell pkg-config where Vulkan is (the SDK does not ship a .pc file)
PC_DIR="$HERE/engine/build/pkgconfig"
mkdir -p "$PC_DIR"
cat > "$PC_DIR/vulkan.pc" <<EOF
sdk=$VK_WIN
Name: vulkan
Description: Vulkan loader (LunarG SDK)
Version: 1.4
Libs: -L\${sdk}/Lib -lvulkan-1
Cflags: -I\${sdk}/Include
EOF

command -v cygpath >/dev/null 2>&1 && PC_DIR="$(cygpath -m "$PC_DIR")"   # pkg-config wants C:/... paths
PKG_CONFIG_PATH="$PC_DIR${PKG_CONFIG_PATH:+;$PKG_CONFIG_PATH}"
if [ -n "$SDL2_DIR" ]; then
    PKG_CONFIG_PATH="$PKG_CONFIG_PATH;$SDL2_DIR/lib/pkgconfig"
fi
export PKG_CONFIG_PATH
export PATH="$VK_BIN:$PATH"

if ! pkg-config --exists sdl2; then
    echo "SDL2 not found. In the MSYS2 MINGW64 shell run:"
    echo "    pacman -S mingw-w64-x86_64-SDL2"
    exit 1
fi

# FSR and DLSS are built in when their SDKs are given (see the README):
#   FSR_SDK  the AMD FidelityFX SDK's Kits/FidelityFX folder
#   SL_SDK   the unzipped NVIDIA Streamline SDK
[ -n "$FSR_SDK" ] && set -- "FSR_SDK=$FSR_SDK" "$@"
[ -n "$SL_SDK" ] && set -- "SL_SDK=$SL_SDK" "$@"

cd "$HERE/engine"
make play CXX=g++ "$@"

# the game needs SDL2.dll next to the exe
SDL_BIN="$(pkg-config --variable=prefix sdl2)/bin"
cp -f "$SDL_BIN/SDL2.dll" build/
echo
echo "Done: $HERE/engine/build/omk-play.exe"
