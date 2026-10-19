#!/usr/bin/env bash
# Off-device equivalence test of led-fade against sm8550-sleep's old python
# fade (fade.py), see scenario.py. Root in the build VM; the rootfs is bound
# read-only and /sys and /run are throwaway fakes.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <outdir> [CASE...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
OUT="${3:?outdir}"
shift 3
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
rm -f "$OUT"/*.wlog "$OUT"/*.json
b="$(mktemp -d)"
trap 'rm -rf "$b"' EXIT
cp "$HERE"/scenario.py "$HERE"/fade.py "$HERE"/wlog.c "$HERE"/vclock.c "$b/"
cp "$BIN" "$b/pbos-platformd"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$b" /tmp/b --chdir /tmp/b --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -shared -fPIC -o wlog.so wlog.c -ldl && gcc -O2 -shared -fPIC -o vclock.so vclock.c'
cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || read -ra cases <<<"$(python3 "$b/scenario.py" --list)"
for c in "${cases[@]}"; do
  for run in virt.py virt.cpp real.py real.cpp; do
    mode=${run%.*} impl=${run#*.}
    t="$(mktemp -d)"
    mkdir -p "$t"/{sys,run}
    bwrap --ro-bind "$R" / --proc /proc --dev /dev --bind "$t/sys" /sys --bind "$t/run" /run --tmpfs /tmp \
      --bind "$b" /tmp/t --bind "$OUT" /tmp/out --unshare-pid --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin \
      python3 /tmp/t/scenario.py "$impl" "$c" /tmp/out "$mode" || echo "!! $c ($run) failed"
    rm -rf "$t"
  done
done
python3 "$HERE/compare.py" "$OUT" "${cases[@]}"
