#!/usr/bin/env bash
# Off-device equivalence test of the thor-desktop-brightness module against
# the Python sm8550-thor-desktop-brightness (see equivalence.py): each on a
# private session bus (dbus-daemon from the rootfs) with a fake PowerDevil.
# Root in the build VM; the rootfs is bound read-only, /sys, /run, /tmp and
# /var/lib/steamos-sm8550 are throwaway fakes.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-thor-desktop-brightness> <outdir> [SCENARIO...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-thor-desktop-brightness}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
cp "$HERE/equivalence.py" "$HERE/fake_powerdevil.py" "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/sm8550-thor-desktop-brightness"
box() {
  bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --tmpfs /sys \
    --tmpfs /var/lib/steamos-sm8550 \
    --bind "$t" /tmp/t --bind "$OUT" /tmp/out \
    --unshare-pid --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin "$@"
}
rc=0
for impl in py cpp; do
  box python3 /tmp/t/equivalence.py run "$impl" /tmp/out/"$impl".json "$@" || rc=1
done
box python3 /tmp/t/equivalence.py compare /tmp/out/py.json /tmp/out/cpp.json || rc=1
exit $rc
