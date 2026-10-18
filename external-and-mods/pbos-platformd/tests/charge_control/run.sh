#!/usr/bin/env bash
# Run the charge-control equivalence test in a SteamOS rootfs (read-only),
# as root in the build VM: the Python daemon against pbos-platformd builds.
#
# Usage: run.sh <rootfs> <sm8550-charge-control script> <binary> [<binary>...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"; SCRIPT="$(realpath "${2:?script}")"; shift 2
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE/equivalence.py" "$work/"
cp "$SCRIPT" "$work/sm8550-charge-control"
printf 'retroidpocket,rpnova\0qcom,sm8550\0' > "$work/compatible"
i=0
for b in "$@"; do cp "$b" "$work/bin$i"; i=$((i+1)); done
box() {
  bwrap --ro-bind "$R" / --bind "$work" /t --dev /dev --proc /proc --tmpfs /tmp --tmpfs /sys \
    --ro-bind "$work/compatible" /sys/firmware/devicetree/base/compatible \
    --unshare-pid --die-with-parent --chdir /t --setenv PATH /usr/bin "$@"
}
S="resume"
ALL="oem_hold,oem_limit_bypass_slow,odin2_limit80,thresholds_first_run,battmgr_late,give_up"
box python3 /t/equivalence.py run py /t/sm8550-charge-control - /t/py.json "$ALL" &
for ((j=0; j<i; j++)); do
  box python3 /t/equivalence.py run cpp /t/sm8550-charge-control /t/bin$j /t/cpp$j.json "$ALL" &
  box python3 /t/equivalence.py run cpp /t/sm8550-charge-control /t/bin$j /t/resume$j.json "$S" &
done
wait
rc=0
cpps=(); for ((j=0; j<i; j++)); do cpps+=("/t/cpp$j.json"); done
box python3 /t/equivalence.py compare /t/py.json "${cpps[@]}" || rc=1
for ((j=0; j<i; j++)); do
  box python3 -c '
import json, sys
s = json.load(open(sys.argv[1]))["resume"]["snapshots"]
cn = "sys/bus/auxiliary/devices/pmic_glink.power-supply.0/retroid_charge_now"
got = [x[cn] for _, x in s]
ok = got[2] == "1" and got[3] == "0" and got[4] == "1" and "run/sm8550-charge-held" not in s[4][1]
print(("PASS" if ok else "FAIL") + " resume (bin%s): charge_now per step %s" % (sys.argv[2], got))
sys.exit(0 if ok else 1)' "/t/resume$j.json" "$j" || rc=1
done
mkdir -p "$HERE/out" 2>/dev/null && cp "$work"/*.json "$HERE/out/" 2>/dev/null || true
exit $rc
