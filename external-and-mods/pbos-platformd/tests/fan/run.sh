#!/usr/bin/env bash
# Off-device equivalence test of the fan module against the Python
# sm8550-fand (see equivalence.py). Root in the build VM; the
# rootfs is bound read-only, and /sys, /etc, /usr/share/sm8550-fand and
# /var/lib/steamos-sm8550 are throwaway fakes.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-fand> <fan.conf> <outdir> [CASE...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-fand}"
CONF="${4:?fan.conf}"
OUT="${5:?outdir}"
shift 5
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
mkdir "$t/sys"
cp -a "$R/etc" "$t/etc"  # a writable copy (this bwrap has no --tmp-overlay)
cp "$HERE/equivalence.py" "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/sm8550-fand"
cp "$CONF" "$t/fan.conf"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run \
  --bind "$t/sys" /sys \
  --bind "$t/etc" /etc --tmpfs /usr/share/sm8550-fand \
  --tmpfs /var/lib/steamos-sm8550 \
  --bind "$t" /tmp/t --bind "$OUT" /tmp/out \
  --unshare-pid --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin \
  python3 /tmp/t/equivalence.py /tmp/t/sm8550-fand /tmp/t/pbos-platformd /tmp/t/fan.conf "$@"
