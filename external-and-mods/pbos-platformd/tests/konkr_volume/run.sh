#!/usr/bin/env bash
# Off-device equivalence test: the Python konkr-volume and pbos-platformd's
# konkr-volume, each against a real PipeWire (a null sink under the speaker
# sink's name) and a real ALSA card (snd-dummy, card 0) given the Pocket
# FIT's speaker controls as user controls. Compares the settled state (bar,
# PA, both WSA gains, the node's softVolumes) after each step. Root (the
# card); the rootfs is bound read-only.
#
# The VM needs snd-dummy loaded as card 0 (sudo insmod snd-dummy.ko.zst).
# Usage: sudo run.sh <rootfs> <pbos-platformd> <konkr-volume> <outdir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?konkr-volume}"
OUT="${4:?outdir}"
grep -q Dummy /proc/asound/card0/id 2>/dev/null || { echo "card 0 is not snd-dummy" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"; chmod 0755 "$t"
trap 'rm -rf "$t"' EXIT
cp "$HERE"/mkctl.c "$HERE"/inner.sh "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/konkr-volume"
printf 'KONKR Pocket FIT\0' >"$t/model"
bw() {
  bwrap --ro-bind "$R" / --dev-bind /dev /dev --proc /proc --ro-bind /sys /sys --tmpfs /tmp \
    --tmpfs /sys/firmware/devicetree --perms 0755 --dir /sys/firmware/devicetree/base \
    --ro-bind "$t/model" /sys/firmware/devicetree/base/model \
    --bind "$t" /tmp/t --bind "$OUT" /tmp/out --die-with-parent --setenv PATH /usr/bin "$@"
}
bw sh -c 'cd /tmp/t && gcc -O2 -Wall -o mkctl mkctl.c -lasound'
for impl in python cpp; do
  bw /bin/bash /tmp/t/inner.sh "$impl" >"$OUT/$impl.run" 2>&1 || echo "$impl: runner failed"
done
grep -v '^fast ' "$OUT/python.snap" >"$OUT/python.settled"
grep -v '^fast ' "$OUT/cpp.snap" >"$OUT/cpp.settled"
if diff "$OUT/python.settled" "$OUT/cpp.settled" >"$OUT/settled.diff"; then
  echo "SAME ($(wc -l <"$OUT/cpp.settled") settled states)"
else
  echo "DIFF (see $OUT/settled.diff)"; cat "$OUT/settled.diff"
fi
echo "--- fast (0.3 s after a reset)"; paste -d'|' <(grep '^fast ' "$OUT/python.snap") <(grep '^fast ' "$OUT/cpp.snap")
echo "--- control events / CPU"; cat "$OUT/python.stats" "$OUT/cpp.stats"
