#!/usr/bin/env bash
# Off-device equivalence test of the thor-controls module against the Python
# sm8550-thor-controlsd (see equivalence.py). Root in the build VM; the
# rootfs is bound read-only, and /sys, /run, /etc's fan config,
# /usr/share/sm8550-fand and /var/lib/steamos-sm8550 are throwaway fakes.
# systemctl is a stub that logs its arguments.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-thor-controlsd> <fan.conf> <outdir> [SCENARIO...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-thor-controlsd}"
CONF="${4:?fan.conf}"
OUT="${5:?outdir}"
shift 5
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
mkdir "$t/bin"
cp "$HERE/equivalence.py" "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/sm8550-thor-controlsd"
cp "$CONF" "$t/fan.conf"
cat >"$t/bin/systemctl" <<'STUB'
#!/bin/sh
echo "systemctl $*" >>/tmp/t/systemctl.log
STUB
chmod +x "$t/bin/systemctl"
box() {
  bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --tmpfs /sys \
    --tmpfs /etc --ro-bind "$R/etc/group" /etc/group --ro-bind "$R/etc/passwd" /etc/passwd \
    --ro-bind "$R/etc/nsswitch.conf" /etc/nsswitch.conf --ro-bind "$R/etc/ld.so.cache" /etc/ld.so.cache \
    --tmpfs /usr/share/sm8550-fand --tmpfs /var/lib/steamos-sm8550 \
    --bind "$t" /tmp/t --bind "$OUT" /tmp/out \
    --unshare-pid --die-with-parent --chdir /tmp/t --setenv PATH /tmp/t/bin:/usr/bin \
    "$@"
}
rc=0
for impl in py cpp; do
  box python3 /tmp/t/equivalence.py run "$impl" /tmp/out/"$impl".json "$@" || rc=1
done
box python3 /tmp/t/equivalence.py compare /tmp/out/py.json /tmp/out/cpp.json || rc=1
exit $rc
