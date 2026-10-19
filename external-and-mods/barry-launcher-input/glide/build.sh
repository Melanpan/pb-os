#!/usr/bin/env bash
# Build barryglide and its tools (glide-cli, glide-blob) inside a SteamOS
# rootfs, read-only, as ../build.sh does for Barry.Input.
#
# Usage: build.sh <rootfs> <output dir>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output dir}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
for f in /usr/bin/cmake /usr/bin/g++ /usr/bin/ninja; do
  [[ -e "$R$f" ]] || { echo "rootfs needs $f" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cp -r "$HERE"/CMakeLists.txt "$HERE"/*.cpp "$HERE"/*.h "$HERE"/*.inc "$HERE"/tools "$work/src/"
run() {
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin --setenv HOME /tmp "$@"
}
run cmake -S /build/src -B /build/out -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
run cmake --build /build/out
install -d "$OUT"
install -m0644 "$work/out/libbarryglide.a" "$OUT/"
install -m0755 "$work/out/glide-cli" "$work/out/glide-blob" "$OUT/"
echo "built $OUT"
