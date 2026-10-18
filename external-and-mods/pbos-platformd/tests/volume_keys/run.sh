#!/usr/bin/env bash
# Off-device equivalence test: the Python sm8550-volume-keys and
# pbos-platformd's volume-keys, on the same fake devices (uinput) and the
# same key scripts, against a real PipeWire (null sink) from the rootfs.
# Compares the default sink's volume/mute trace of each. Root (uinput, bwrap
# without user namespaces); the rootfs is bound read-only.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-volume-keys> <outdir>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-volume-keys}"
OUT="${4:?outdir}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"; chmod 0755 "$t"  # the user-run daemon reads it
trap 'rm -rf "$t"' EXIT
cp "$HERE"/uinput.py "$HERE"/sinkrec.c "$HERE"/inner.sh "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/sm8550-volume-keys"
printf 'ayn,odin2\0qcom,sm8550\0' >"$t/compat"
bw() {
  bwrap --ro-bind "$R" / --dev-bind /dev /dev --proc /proc --ro-bind /sys /sys --tmpfs /tmp \
    --bind "$t" /tmp/t --bind "$OUT" /tmp/out --die-with-parent --setenv PATH /usr/bin "$@"
}
bw sh -c 'cd /tmp/t && gcc -O2 -Wall -o sinkrec sinkrec.c -lpulse'
modprobe uinput 2>/dev/null || true
rc=0
for scen in ${SCENARIOS:-s1 s2 s3 s4}; do  # (sudo SCENARIOS=... run.sh)
  dt=()
  [[ $scen == s2 || $scen == s5 ]] && dt=(--tmpfs /sys/firmware/devicetree --perms 0755 --dir /sys/firmware/devicetree/base --ro-bind "$t/compat" /sys/firmware/devicetree/base/compatible)
  for impl in python cpp; do
    bw "${dt[@]}" /bin/bash /tmp/t/inner.sh "$scen" "$impl" >"$OUT/$scen.$impl.run" 2>&1 || echo "$scen $impl: runner failed"
  done
  # The volume and the mute state each on their own: a step's unmute and its
  # volume can land in one sample or two, depending on how fast they follow.
  for impl in python cpp; do
    awk '{ m = $NF; $NF = ""; if ($0 != v) print "volume " $0; v = $0 }' "$OUT/$scen.$impl.trace" >"$OUT/$scen.$impl.steps"
    awk '{ if ($NF != m) print $NF; m = $NF }' "$OUT/$scen.$impl.trace" >>"$OUT/$scen.$impl.steps"
  done
  full=same
  cmp -s "$OUT/$scen.python.trace" "$OUT/$scen.cpp.trace" || full="samples differ"
  if diff "$OUT/$scen.python.steps" "$OUT/$scen.cpp.steps" >"$OUT/$scen.diff"; then
    echo "$scen SAME ($(grep -c volume "$OUT/$scen.cpp.steps") volumes; full trace: $full)"
  else
    echo "$scen DIFF (see $OUT/$scen.diff)"
    rc=1
  fi
done
exit $rc
