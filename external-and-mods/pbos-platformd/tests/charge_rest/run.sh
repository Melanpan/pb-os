#!/usr/bin/env bash
# Off-device equivalence test of pbos-platformd charge-rest against the
# Python sm8550-charge-rest (see scenario.py and cli.py). Root in the build
# VM; the rootfs is bound read-only, and /sys, /run, /dev/input,
# /var/lib/pbos-utils and /usr/lib/konkr are throwaway fakes, in their own
# pid and network namespaces; systemd-sleep, systemctl, konkr-suspend and
# konkr-sleep-mode are stubs that log their argv.
#
# Nothing real is touched: nothing suspends, no cgroup is frozen, no CPU
# goes offline, only this test's own fake processes get SIGSTOP, no input
# device is made (the power key and the lid are FIFOs) and the uevents stay
# in the test's network namespace.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-charge-rest (Python)> <outdir> [CASE...]
#   REAL=1   also the timing runs (real clock) of a few cases
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-charge-rest}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
b="$(mktemp -d)"
trap 'rm -rf "$b"' EXIT
cp "$HERE"/scenario.py "$HERE"/cli.py "$HERE"/wlog.c "$HERE"/vclock.c "$HERE"/procs.c "$b/"
cp "$BIN" "$b/pbos-platformd"
cp "$PY" "$b/sm8550-charge-rest"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$b" /tmp/b --chdir /tmp/b --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -shared -fPIC -o wlog.so wlog.c && gcc -O2 -shared -fPIC -o vclock.so vclock.c && gcc -O2 -o procs procs.c'
for n in reaper game other; do cp "$b/procs" "$b/$n"; done
mkdir -p "$b/bin"
cp "$HERE/stub" "$b/stub"
cp "$HERE/stub" "$b/bin/systemctl"

box() {  # box CTLDIR SYSDIR RUNDIR DEVINPUT VARLIB CMD...
  local t="$1"; shift
  bwrap --ro-bind "$R" / --proc /proc --dev /dev --bind "$t/devinput" /dev/input \
    --bind "$t/sys" /sys --bind "$t/run" /run --tmpfs /var/lib --bind "$t/varlib" /var/lib/pbos-utils \
    --tmpfs /tmp --bind "$t/ctl" /tmp/ctl --bind "$b" /tmp/t --bind "$OUT" /tmp/out \
    --tmpfs /usr/lib/konkr --ro-bind "$b/stub" /usr/lib/konkr/konkr-suspend \
    --ro-bind "$b/stub" /usr/lib/konkr/konkr-sleep-mode --ro-bind "$b/stub" /usr/lib/systemd/systemd-sleep \
    --unshare-pid --unshare-net --cap-add ALL --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin "$@"
}
fresh() { t="$(mktemp -d)"; mkdir -p "$t"/{sys,run,devinput,ctl,varlib}; echo "$t"; }

cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || cases=(exec-konkr exec-sleep-battery fit-standby watch-wake watch-keyfifo doorbells-key
  doorbell-plug-full-key rest-unplug-sleep rest-replug-usr1 rest-lid rest-full-usr1 rest-konkr-key rest-konkr-bound
  rest-no-htr rest-no-i2c rest-disabled rest-sigterm rest-sigint freeze-timeout preview preview-term preview-nolights)
for c in "${cases[@]}"; do
  [[ $c == cli ]] && continue
  for impl in py cpp; do
    t="$(fresh)"
    box "$t" python3 /tmp/t/scenario.py "$impl" "$c" || echo "!! $c ($impl) failed"
    [[ -n "${KEEP:-}" ]] && echo "kept $t" || rm -rf "$t"
  done
done
rc=0
python3 "$HERE/compare.py" "$OUT" "${cases[@]/cli/}" || rc=1
t="$(fresh)"
# The CLI on a Nova's fake tree (stick zones: "lights": true).
box "$t" sh -c 'python3 - <<"PY"
import os
for z, drv in (("rgb:l1", "leds_group_multicolor"),):
    d = f"/sys/class/leds/{z}"
    os.makedirs(f"/sys/bus/platform/drivers/{drv}", exist_ok=True)
    os.makedirs(f"{d}/device", exist_ok=True)
    os.symlink(f"/sys/bus/platform/drivers/{drv}", f"{d}/device/driver")
    open(f"{d}/multi_intensity", "w").write("1 2 3\n")
PY
python3 /tmp/t/cli.py' || rc=1
rm -rf "$t"
if [[ -n "${REAL:-}" ]]; then
  for c in exec-sleep-battery watch-wake rest-sigterm rest-full-usr1 preview; do
    for impl in py cpp; do
      for k in 1 2 3; do
        t="$(fresh)"
        box "$t" env CREST_VCLOCK=0 python3 /tmp/t/scenario.py "$impl" "$c" >/dev/null || echo "!! $c ($impl) failed"
        cp "$OUT/$c.$impl.real.json" "$OUT/$c.$impl.real$k.json"
        rm -rf "$t"
      done
    done
  done
fi
exit $rc
