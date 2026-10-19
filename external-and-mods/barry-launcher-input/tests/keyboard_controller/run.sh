#!/usr/bin/env bash
# KeyboardController's test (keyboard_controller_test.cpp): built and run in a
# SteamOS rootfs (read-only) under bwrap, offscreen, with the repo's packs.
#
# Usage: run.sh <rootfs> <repo root> <work dir>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
REPO="$(cd "${2:?repo root}" && pwd)"
W="${3:?work dir}"

rm -rf "$W"
mkdir -p "$W/src" "$W/shipped"
run() {
  bwrap --ro-bind "$R" / --bind "$W" /t --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --unshare-net --die-with-parent --chdir /t --setenv PATH /usr/bin --setenv HOME /tmp/home \
    --setenv XDG_RUNTIME_DIR /tmp/run --setenv XDG_CONFIG_HOME /tmp/home/.config \
    --setenv XDG_DATA_HOME /tmp/home/.local/share --setenv XDG_CACHE_HOME /tmp/home/.cache \
    --setenv BARRY_KEYBOARDS_DIRS /t/shipped "$@"
}

echo "== build"
cp -r "$SRC"/. "$W/src/"
run cmake -S /t/src -B /t/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARRY_INPUT_KEYBOARD_TEST=ON >/dev/null
run cmake --build /t/out --target keyboard-controller-test >/dev/null

OV="$REPO/sm8550-overlay/usr/share/barry_launcher"
cp -r "$OV/keyboards/en" "$W/shipped/"
cp -r "$OV/keyboards-extra/en-thumbkey" "$W/shipped/"

echo "== test"
run /t/out/keyboard-controller-test
