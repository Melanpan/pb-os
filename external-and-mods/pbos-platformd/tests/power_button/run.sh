#!/usr/bin/env bash
# Off-device equivalence test of the power-button module against the Python
# sm8550-powerbuttond (see equivalence.py). Root in the build VM (uinput);
# the rootfs is bound read-only, runuser/systemctl/pgrep are logging stubs,
# /run and the device tree's compatible are fakes.
#
# WARNING: the fake power key is a real input device to the host: the VM's
# own systemd-logind acts on KEY_POWER (HandlePowerKey=poweroff) and powers
# the VM off. The whole test therefore runs under a logind inhibitor
# (handle-power-key, block) and refuses to run without one.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-powerbuttond> <outdir> [SCENARIO...]
set -euo pipefail
if [[ "${PB_TEST_INHIBITED:-}" != 1 ]]; then
  command -v systemd-inhibit >/dev/null || { echo "systemd-inhibit required (see WARNING)" >&2; exit 1; }
  exec env PB_TEST_INHIBITED=1 systemd-inhibit --what=handle-power-key:handle-suspend-key --mode=block \
    --who=pbos-platformd-test --why="fake power key" bash "$0" "$@"
fi
# The inhibitor is really held (else logind would power the VM off).
systemd-inhibit --list --no-pager 2>/dev/null | grep -q pbos-platformd-test \
  || { echo "no handle-power-key inhibitor held; refusing (see WARNING)" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-powerbuttond}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
mkdir "$t/run" "$t/ctl"
cp "$HERE/equivalence.py" "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$PY" "$t/sm8550-powerbuttond"
printf 'ayn,thor\0qcom,sm8550\0' >"$t/compat"
# A stub: who called it (the parent's name), what, and the environment the
# commands depend on. pgrep finds plasmashell while /tmp/ctl/desktop exists.
cat >"$t/stub" <<'EOF'
#!/bin/sh
echo "$(cat /proc/$PPID/comm) $(basename "$0") $* | HOME=$HOME USER=$USER PATH=$PATH DISPLAY=$DISPLAY XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS" >>/tmp/ctl/log
EOF
cat >"$t/pgrep" <<'EOF'
#!/bin/sh
[ -e /tmp/ctl/desktop ]
EOF
chmod +x "$t/stub" "$t/pgrep"
modprobe uinput 2>/dev/null || true
bwrap --ro-bind "$R" / --dev-bind /dev /dev --proc /proc --ro-bind /sys /sys \
  --tmpfs /sys/firmware --ro-bind "$t/compat" /sys/firmware/devicetree/base/compatible \
  --tmpfs /tmp --bind "$t/run" /run --bind "$t/ctl" /tmp/ctl \
  --ro-bind "$t/stub" /usr/bin/runuser --ro-bind "$t/stub" /usr/bin/systemctl --ro-bind "$t/pgrep" /usr/bin/pgrep \
  --bind "$t" /tmp/t --bind "$OUT" /tmp/out \
  --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin \
  python3 /tmp/t/equivalence.py /tmp/t/sm8550-powerbuttond /tmp/t/pbos-platformd "$@"
