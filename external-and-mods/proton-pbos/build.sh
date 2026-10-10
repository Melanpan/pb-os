#!/bin/bash
# Proton PB-OS: build Wine's X11 driver (winex11.so, aarch64-unix) from Valve's
# Proton source with the patches here, in Valve's own arm64 Proton SDK image.
# Run on an arm64 Linux host with Docker (the build VM), as root:
#   external-and-mods/proton-pbos/build.sh [WORKDIR]      (default /work/proton-pbos)
# Output: WORKDIR/winex11.so (stripped) for files/lib/wine/aarch64-unix/ of a
# copy of Valve's "Proton 11.0 (ARM64)" of the same version (PROTON_TAG).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W="${1:-/work/proton-pbos}"
PROTON_TAG=proton-11.0-2e   # Valve's tool version: "proton-11.0-2e-arm64"
SDK=registry.gitlab.steamos.cloud/proton/steamrt4/sdk/arm64-llvm:4.0.20260331.220802-2
J="${J:-$(nproc)}"

mkdir -p "$W"
if [[ ! -d "$W/src/.git" ]]; then
  git clone --depth 1 --branch "$PROTON_TAG" https://github.com/ValveSoftware/Proton.git "$W/src"
  git -C "$W/src" submodule update --init --depth 1 wine
fi
docker pull -q "$SDK"
cp "$HERE"/*.patch "$W/"

docker run --rm -v "$W:$W" -e W="$W" -e J="$J" "$SDK" bash -euo pipefail -c '
  rm -rf "$W/wine" "$W/build"; cp -a "$W/src/wine" "$W/wine"; cd "$W/wine"
  for p in "$W"/*.patch; do patch -p1 < "$p"; done
  # As Proton'"'"'s Makefile.in does before configuring wine.
  XDG_CACHE_HOME="$W/cache" dlls/winevulkan/make_vulkan -x vk.xml -X video.xml
  tools/make_specfiles || true
  tools/make_requests
  autoreconf -fi >/dev/null
  mkdir -p "$W/build"; cd "$W/build"
  CFLAGS="-O2 -g -march=armv8.2-a -mtune=cortex-x3" "$W/wine/configure" -C \
    --host=aarch64-linux-gnu --disable-tests --with-mingw=clang \
    --enable-archs=arm64ec,aarch64,i386 >configure.log 2>&1
  make -j"$J" __tooldeps__ >make-tools.log 2>&1
  make -j"$J" dlls/winex11.drv/winex11.so >make-x11.log 2>&1
  strip -o "$W/winex11.so" dlls/winex11.drv/winex11.so'
ls -la "$W/winex11.so"
