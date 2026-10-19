#!/usr/bin/env bash
# Build sm8550-boostd inside a SteamOS rootfs (as pbos-qam/build.sh does):
# boostd.cpp plus pbos-qamd's event loop and helpers (loop, util, json).
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QAM="$HERE/../pbos-qam/daemon"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/g++" && -f "$R/usr/include/systemd/sd-bus.h" && -f "$R/usr/include/linux/cn_proc.h" ]] \
  || { echo "rootfs needs g++, systemd and the kernel headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE/boostd.cpp" "$QAM"/{loop,util,json}.{cpp,h} "$work/"
run() {
  # Read-only: the build writes nothing into the rootfs.
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin "$@"
}
run g++ -std=c++20 -O2 -g0 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -Wall -Wextra -Werror -fno-plt -pthread \
  -o sm8550-boostd boostd.cpp loop.cpp util.cpp json.cpp -lsystemd
run strip sm8550-boostd
install -D -m0755 "$work/sm8550-boostd" "$OUT"
echo "built $OUT"
