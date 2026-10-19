#!/usr/bin/env bash
# Build pbos-platformd inside a SteamOS rootfs (as pbos-qam/build.sh does):
# its modules plus pbos-qamd's event loop and helpers (loop, util, json).
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QAM="$HERE/../pbos-qam/daemon"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/g++" && -f "$R/usr/include/systemd/sd-bus.h" && -f "$R/usr/include/pulse/pulseaudio.h" \
   && -f "$R/usr/include/alsa/asoundlib.h" && -f "$R/usr/include/pipewire-0.3/pipewire/pipewire.h" ]] \
  || { echo "rootfs needs g++ and the systemd, libpulse, ALSA and PipeWire headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE"/*.cpp "$HERE"/*.h "$QAM"/{loop,util,json}.{cpp,h} "$work/"
run() {
  # Read-only: the build writes nothing into the rootfs.
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin "$@"
}
srcs=()
for f in "$work"/*.cpp; do srcs+=("$(basename "$f")"); done
run g++ -std=c++20 -O2 -g0 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -Wall -Wextra -Werror -fno-plt -pthread -D_REENTRANT \
  -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 \
  -o pbos-platformd "${srcs[@]}" -lsystemd -lpulse -lasound -lpipewire-0.3
run strip pbos-platformd
install -D -m0755 "$work/pbos-platformd" "$OUT"
echo "built $OUT"
