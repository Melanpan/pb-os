#!/usr/bin/env bash
# The brightness maths of thor_backlight.h against the Python daemon's, over
# a grid (every Steam value, slider positions in 1/10000 steps, glides).
# Built in the rootfs with build.sh's flags.
#
# Usage: maths.sh <rootfs>   (root: bwrap)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
mkdir -p "$t/tests/thor_backlight"
cp "$HERE/maths.cpp" "$HERE/maths.py" "$t/tests/thor_backlight/"
cp "$HERE/../../thor_backlight.h" "$t/"
bw() { bwrap --ro-bind "$R" / --bind "$t" /build --dev /dev --proc /proc --tmpfs /tmp --chdir /build --setenv PATH /usr/bin "$@"; }
bw g++ -std=c++20 -O2 -Wall -Wextra -Werror -o maths tests/thor_backlight/maths.cpp
bw ./maths >"$t/cpp.txt"
bw python3 tests/thor_backlight/maths.py >"$t/py.txt"
n=$(wc -l <"$t/cpp.txt")
if cmp -s "$t/cpp.txt" "$t/py.txt"; then
  echo "maths SAME ($n values)"
else
  diff "$t/py.txt" "$t/cpp.txt" | head -20
  echo "maths DIFF ($(diff "$t/py.txt" "$t/cpp.txt" | grep -c '^>') of $n lines)"
  exit 1
fi
