#!/usr/bin/env bash
# Builds MirRoarServer.exe (single static executable with GUI) with MinGW-w64 + CMake.
# Downloads and builds the GUI dependencies (SDL3, Dear ImGui, ImPlot) the first time.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw64/bin:$PATH"
TP=../third_party
SDL_VER=3.4.18
IMGUI_VER=v1.92.9b
IMPLOT_VER=v1.0

fetch() {  # url dir
  if [ ! -d "$TP/$2" ]; then
    echo "download $2..."
    mkdir -p "$TP/$2"
    curl -sSL "$1" | tar -xz -C "$TP/$2" --strip-components=1
  fi
}
fetch "https://github.com/ocornut/imgui/archive/refs/tags/$IMGUI_VER.tar.gz" imgui
fetch "https://github.com/epezent/implot/archive/refs/tags/$IMPLOT_VER.tar.gz" implot
if [ ! -f "$TP/sdl-install/lib/libSDL3.a" ]; then
  fetch "https://github.com/libsdl-org/SDL/archive/refs/tags/release-$SDL_VER.tar.gz" sdl
  echo "build SDL3 (static, video + render + tray only)..."
  cmake -S "$TP/sdl" -B "$TP/sdl-build" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_AUDIO=OFF -DSDL_GPU=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF \
    -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_VULKAN=OFF -DSDL_OPENGLES=OFF -DSDL_OPENGL=OFF \
    -DSDL_RENDER_D3D=OFF -DSDL_RENDER_D3D12=OFF -DSDL_RENDER_VULKAN=OFF -DSDL_RENDER_GPU=OFF \
    -DSDL_VIRTUAL_JOYSTICK=OFF -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_INSTALL_PREFIX="$(cd "$TP" && pwd)/sdl-install" -DCMAKE_C_COMPILER=gcc >/dev/null
  cmake --build "$TP/sdl-build" -j >/dev/null
  cmake --install "$TP/sdl-build" >/dev/null
fi

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_RC_COMPILER=windres >/dev/null
cmake --build build -j
mkdir -p ../dist
cp build/MirRoarServer.exe ../dist/
echo "OK -> dist/MirRoarServer.exe ($(du -h ../dist/MirRoarServer.exe | cut -f1))"
