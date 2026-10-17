#!/usr/bin/env bash
# Build Barry.Input (Barry Launcher's keyboards and trackpad, a QML module)
# inside a SteamOS rootfs, as barry-launcher-imd/build.sh does: against the
# rootfs's Qt, libX11 and libXtst, read-only.
#
# Usage: build.sh <rootfs> <output dir>   (the module's dir: .../qml/Barry/Input)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output dir}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
for f in /usr/bin/cmake /usr/bin/g++ /usr/bin/ninja /usr/lib/cmake/Qt6Qml/Qt6QmlConfig.cmake \
  /usr/lib/cmake/Qt6DBus/Qt6DBusConfig.cmake /usr/include/X11/extensions/XTest.h; do
  [[ -e "$R$f" ]] || { echo "rootfs needs $f (cmake, g++, ninja, Qt 6 Qml/DBus, libXtst)" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
cp "$HERE"/CMakeLists.txt "$HERE"/*.cpp "$HERE"/*.h "$HERE"/*.qml "$work/src/"
run() {
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin --setenv HOME /tmp "$@"
}
run cmake -S /build/src -B /build/out -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
run cmake --build /build/out
mod="$work/out/qml/Barry/Input"
[[ -f "$mod/qmldir" ]] || { echo "no qmldir in $mod" >&2; exit 1; }
lib="$(find "$work/out" -name 'libbarryinput*.so' -print -quit)"
[[ -n "$lib" ]] || { echo "no libbarryinput*.so built" >&2; exit 1; }
rm -rf "$OUT"
install -d "$OUT"
install -m0644 "$mod/qmldir" "$OUT/"
find "$mod" -maxdepth 1 -name '*.qmltypes' -exec install -m0644 {} "$OUT/" \;
install -m0755 "$lib" "$OUT/"
echo "built $OUT"
