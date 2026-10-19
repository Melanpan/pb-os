#!/usr/bin/env bash
# Build pbos-qamd inside a SteamOS rootfs (as barry-launcher-imd/build.sh
# does): the rootfs has g++, libsystemd (sd-bus) and libarchive's headers.
# libarchive is loaded at run time (dlopen), only to unpack a theme or a
# language pack.
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/g++" && -f "$R/usr/include/systemd/sd-bus.h" && -f "$R/usr/include/archive.h" ]] \
  || { echo "rootfs needs g++, systemd and libarchive headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE"/daemon/*.cpp "$HERE"/daemon/*.h "$work/"
run() {
  # Read-only: the build writes nothing into the rootfs.
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin "$@"
}
srcs=()
for f in "$work"/*.cpp; do srcs+=("$(basename "$f")"); done
run g++ -std=c++20 -O2 -g0 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -Wall -Wextra -Werror -fno-plt -pthread \
  -o pbos-qamd "${srcs[@]}" -lsystemd -ldl
run strip pbos-qamd
install -D -m0755 "$work/pbos-qamd" "$OUT"
echo "built $OUT"
