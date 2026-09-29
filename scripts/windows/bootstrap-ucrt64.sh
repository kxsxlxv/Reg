#!/usr/bin/env bash
set -euo pipefail

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
  echo "This script must run in an MSYS2 UCRT64 environment." >&2
  echo "Open 'MSYS2 UCRT64' or run scripts/windows/bootstrap-ucrt64.ps1 from PowerShell." >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

echo "[bootstrap] repo: $repo_root"
echo "[bootstrap] installing/updating required UCRT64 packages"

pacman -S --needed --noconfirm   git   make   nasm   mingw-w64-ucrt-x86_64-toolchain   mingw-w64-ucrt-x86_64-cmake   mingw-w64-ucrt-x86_64-ninja   mingw-w64-ucrt-x86_64-pkgconf   mingw-w64-ucrt-x86_64-vulkan-headers   mingw-w64-ucrt-x86_64-vulkan-loader   mingw-w64-ucrt-x86_64-shaderc

echo "[bootstrap] initializing submodules"
git submodule update --init --recursive

echo "[bootstrap] checking Vulkan shader compiler"
command -v glslc
glslc --version

ffmpeg_prefix="$repo_root/.deps/ffmpeg-ucrt64"
ffmpeg_build="$repo_root/.deps/ffmpeg-build-ucrt64"

if [[ ! -f "$ffmpeg_prefix/include/libavcodec/avcodec.h" ]]; then
  echo "[bootstrap] building bundled FFmpeg with H.264 Vulkan decode"

  rm -rf "$ffmpeg_build"
  mkdir -p "$ffmpeg_build"
  cd "$ffmpeg_build"

  PKG_CONFIG_PATH=/ucrt64/lib/pkgconfig   CFLAGS="-I/ucrt64/include"   CPPFLAGS="-I/ucrt64/include"   LDFLAGS="-L/ucrt64/lib"   "$repo_root/ffmpeg/configure"     --prefix="$ffmpeg_prefix"     --disable-programs     --disable-doc     --enable-shared     --disable-static     --disable-avdevice     --disable-avfilter     --disable-swscale     --disable-swresample     --disable-encoders     --disable-muxers     --enable-muxer=matroska     --disable-decoders     --enable-decoder=h264     --enable-hwaccel=h264_vulkan     --enable-vulkan

  grep -E '^#define CONFIG_H264_VULKAN_HWACCEL 1$' config_components.h

  jobs="${NUMBER_OF_PROCESSORS:-4}"
  make -j"$jobs"
  make install
else
  echo "[bootstrap] reusing existing FFmpeg install: $ffmpeg_prefix"
fi

cd "$repo_root"

echo "[bootstrap] configuring Reg"
cmake -S . -B build-win -G Ninja   -DCMAKE_BUILD_TYPE=RelWithDebInfo   -DFFMPEG_ROOT="$ffmpeg_prefix"   -DREG_ENABLE_VALIDATION=OFF   -DREG_BUILD_TESTS=ON   -DVulkan_INCLUDE_DIR=/ucrt64/include   -DVulkan_LIBRARY=/ucrt64/lib/libvulkan-1.dll.a

echo "[bootstrap] building Reg"
cmake --build build-win --parallel

echo "[bootstrap] running unit tests"
export PATH="$ffmpeg_prefix/bin:$repo_root/build-win:/ucrt64/bin:$PATH"
ctest --test-dir build-win --output-on-failure

cat <<EOF

[bootstrap] PASS

glslc:
  $(command -v glslc)

FFmpeg:
  $ffmpeg_prefix

Probe:
  $repo_root/build-win/reg_probe.exe

Run the exact-frame identity probe with:
  export PATH="$ffmpeg_prefix/bin:$repo_root/build-win:/ucrt64/bin:\$PATH"
  ./build-win/reg_probe.exe \
    --url rtsp://10.97.83.121:8555/reg \
    --identity-probe-frames 300 \
    --disable-overlay \
    --disable-recorder \
    --disable-telemetry
EOF
