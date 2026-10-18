#!/usr/bin/env bash
# Build barry_capture (the quick settings' Record button: the top screen to
# an MP4) inside a SteamOS rootfs, as barry-launcher-imd's build.sh does: the
# rootfs has gcc, pkg-config and the PipeWire and GStreamer headers.
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/gcc" && -x "$R/usr/bin/pkg-config" && -d "$R/usr/include/pipewire-0.3" \
   && -d "$R/usr/include/gstreamer-1.0/gst/app" ]] \
  || { echo "rootfs needs gcc, pkg-config and the PipeWire and GStreamer headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE/barry_capture.c" "$work/"
bwrap --bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
  --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -Wall -Wextra -Werror -o barry_capture barry_capture.c \
    $(pkg-config --cflags --libs libpipewire-0.3 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)'
install -D -m0755 "$work/barry_capture" "$OUT"
echo "built $OUT"
