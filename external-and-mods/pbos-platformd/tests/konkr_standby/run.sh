#!/usr/bin/env bash
# Off-device equivalence test of pbos-platformd konkr-standby against the
# Python konkr-standby (see scenario.py). Root in the build VM; the rootfs is
# bound read-only, and /sys, /etc, /run, /dev/input and /proc/self/cgroup
# are throwaway fakes; konkr-sleep and systemctl are stubs that log their
# argv.
#
# Nothing real is touched: no cgroup is frozen, no CPU goes offline, only
# this test's own fake processes (in their own pid namespace) get SIGSTOP,
# and no input device is made (the power key is a FIFO).
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <konkr-standby (Python)> <outdir> [CASE...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?konkr-standby}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
b="$(mktemp -d)"
trap 'rm -rf "$b"' EXIT
cp "$HERE"/scenario.py "$HERE"/wlog.c "$HERE"/procs.c "$b/"
cp "$BIN" "$b/pbos-platformd"
cp "$PY" "$b/konkr-standby.py"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$b" /tmp/b --chdir /tmp/b --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -shared -fPIC -o wlog.so wlog.c -ldl && gcc -O2 -o procs procs.c'
for n in reaper game other; do cp "$b/procs" "$b/$n"; done
mkdir -p "$b/bin"
cp "$HERE/stub" "$b/bin/systemctl"
cp "$HERE/stub" "$b/konkr-sleep"
cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || cases=(key usr1 freeze-timeout no-key)
for c in "${cases[@]}"; do
  for impl in py cpp; do
    t="$(mktemp -d)"
    mkdir -p "$t"/{sys,run,devinput,ctl}
    cp -a "$R/etc" "$t/etc"
    echo "== $c ($impl)"
    if ! bwrap --ro-bind "$R" / --proc /proc --dev /dev --bind "$t/devinput" /dev/input \
        --bind "$t/sys" /sys --bind "$t/etc" /etc --bind "$t/run" /run \
        --tmpfs /tmp --bind "$t/ctl" /tmp/ctl --bind "$b" /tmp/t --bind "$OUT" /tmp/out \
        --tmpfs /usr/lib/konkr --ro-bind "$b/konkr-sleep" /usr/lib/konkr/konkr-sleep \
        --unshare-pid --cap-add CAP_SYS_ADMIN --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin \
        python3 /tmp/t/scenario.py "$impl" "$c"; then
      echo "!! $c ($impl) failed"
    fi
    [[ -n "${KEEP:-}" ]] && echo "kept $t" || rm -rf "$t"
  done
done
python3 "$HERE/compare.py" "$OUT" "${cases[@]}"
