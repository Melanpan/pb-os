#!/usr/bin/env bash
# Off-device check of the memguard module (root, in the Mini's Linux VM):
# FIFOs in place of the two PSI triggers (each write is one firing), a stub
# systemctl over fake units, and sleep processes in fake user-unit cgroups
# for the OOM pins. The last part fires the real kernel trigger: a process
# held under memory.high in a cgroup of its own stalls on reclaim.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd binary>
set -euo pipefail
R="$(cd "${1:?rootfs}" && pwd)"
BIN="$(realpath "${2:?pbos-platformd}")"
T="$(mktemp -d)"
CG=/sys/fs/cgroup/pbos-memguard-test
pids=()
cleanup() {
  pkill -TERM -f "^$T/pbos-platformd" 2>/dev/null || true
  kill "${pids[@]}" 2>/dev/null || true
  [[ -d "$CG" ]] && echo 1 >"$CG/cgroup.kill"
  wait 2>/dev/null || true
  sleep 0.5
  find "$CG" -depth -type d -exec rmdir {} \; 2>/dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT
cp "$BIN" "$T/pbos-platformd"
mkdir -p "$T/units" "$CG"
echo +memory >"$CG/cgroup.subtree_control"
mkfifo "$T/early" "$T/act"

# systemctl: a unit is active while $T/units/<unit> exists.
cat >"$T/systemctl" <<STUB
#!/bin/bash
echo "\$*" >>"$T/calls"
case "\$1" in
  is-active) [[ -e "$T/units/\${@: -1}" ]] ;;
  stop) rm -f "$T/units/\$2" ;;
  start) touch "$T/units/\${@: -1}" ;;
  show) [[ -e "$T/units/\${@: -1}" ]] && cat "$T/mainpid" || echo 0 ;;
esac
STUB
chmod +x "$T/systemctl"

fail=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
calls() { grep -c "^$1" "$T/calls" 2>/dev/null || true; }
fire() { timeout 2 bash -c "echo x >'$T/$1'" || bad "guard not reading $1"; }

# A process in a user unit's cgroup (as on the device: .../user@1000.service/...).
in_unit() {
  mkdir -p "$CG/user@1000.service/$1"
  sleep 1000 >/dev/null &
  local p=$!
  echo "$p" >"$CG/user@1000.service/$1/cgroup.procs"
  echo 100 >"/proc/$p/oom_score_adj"
  echo "$p"
}
pw=$(in_unit pipewire.service)
gs=$(in_unit gamescope-session.service)
st=$(in_unit steam.service)
app=$(in_unit app.slice/barry-app-signal.scope)
sleep 1000 &
loader=$!
pids+=("$loader")
echo 999 >"/proc/$loader/oom_score_adj"
echo "$loader" >"$T/mainpid"
touch "$T/units/gpu-trace.service" "$T/units/plugin_loader.service"

start_guard() {
  bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$T" "$T" \
    --die-with-parent \
    --setenv PBOS_MEMGUARD_SYSTEMCTL "$T/systemctl" --setenv PBOS_MEMGUARD_CALM_S 5 \
    "$@" "$T/pbos-platformd" --system --only memguard 2>>"$T/log" &
  gpid=$!
  pids+=("$gpid")
  sleep 1
  pgrep -f "$T/pbos-platformd" >/dev/null || { cat "$T/log"; echo "FAIL guard did not start"; exit 1; }
}
# The VM has RAM free: LOW_PCT 100 lets the streaks act (the gate is checked last).
start_guard --setenv PBOS_MEMGUARD_PSI_EARLY "$T/early" --setenv PBOS_MEMGUARD_PSI_ACT "$T/act" \
  --setenv PBOS_MEMGUARD_LOW_PCT 100

stop_guard() {
  pkill -TERM -f "^$T/pbos-platformd" || true
  wait "$gpid" 2>/dev/null || true
}
adj() { cat "/proc/$1/oom_score_adj"; }
check "pin: pipewire -500" '[[ $(adj $pw) == -500 ]]'
check "pin: gamescope -500" '[[ $(adj $gs) == -500 ]]'
check "pin: steam stays 100 (games inherit it)" '[[ $(adj $st) == 100 ]]'
check "pin: Barry app scope untouched" '[[ $(adj $app) == 100 ]]'
check "pin: Decky loader 1000 (above its backends)" '[[ $(adj $loader) == 1000 ]]'
check "idle: nothing stopped" '[[ $(calls stop) == 0 ]]'

fire act; sleep 1
check "one act firing: nothing stopped" '[[ $(calls stop) == 0 ]]'
fire act; sleep 1
check "streak of two: gpu-trace stopped first" '[[ $(calls stop) == 1 && ! -e $T/units/gpu-trace.service ]]'
fire act; sleep 4
check "a lone firing after the gap: nothing more" '[[ $(calls stop) == 1 ]]'
# The calm timer (5 s) was pushed back by that firing; keep it away.
fire early; sleep 1; fire act; sleep 1; fire act; sleep 1
check "next streak: Decky stopped" '[[ $(calls stop) == 2 && ! -e $T/units/plugin_loader.service ]]'
fire act; sleep 1; fire act; sleep 0.5
check "next streak: nothing left (logged)" '[[ $(calls stop) == 2 ]] && grep -q "nothing left to stop" "$T/log"'
for _ in 1 2 3 4; do fire early; sleep 1; done
check "early firings keep the units stopped" '[[ $(calls start) == 0 ]]'
sleep 5.5
check "calm: both started again" '[[ -e $T/units/gpu-trace.service && -e $T/units/plugin_loader.service ]]'
check "calm: Decky started before gpu-trace" \
  '[[ "$(grep ^start "$T/calls" | tr "\n" " ")" == "start plugin_loader.service start gpu-trace.service " ]]'
check "freed memory logged" 'grep -q "MemAvailable .* -> .* MB" "$T/log"'

# gpu-trace not running: the first streak goes to Decky; calm starts only Decky.
rm -f "$T/units/gpu-trace.service" "$T/calls"
fire act; sleep 0.5; fire act; sleep 1
check "gpu-trace off: Decky stopped in the same streak" '[[ $(calls stop) == 1 && ! -e $T/units/plugin_loader.service ]]'
sleep 6
check "calm: only Decky started (gpu-trace stays off)" \
  '[[ -e $T/units/plugin_loader.service && ! -e $T/units/gpu-trace.service ]]'
stop_guard

# The default gate (10%): with the VM's RAM free a streak stops nothing.
touch "$T/units/gpu-trace.service"
rm -f "$T/calls"
start_guard --setenv PBOS_MEMGUARD_PSI_EARLY "$T/early" --setenv PBOS_MEMGUARD_PSI_ACT "$T/act"
fire act; sleep 0.5; fire act; sleep 1
check "RAM free: a streak stops nothing (logged)" \
  '[[ $(calls stop) == 0 ]] && grep -q "nothing stopped" "$T/log"'
stop_guard

# The real kernel trigger: 400 MB touched under memory.high=48M (no swap in
# the VM: the allocator stalls on reclaim and throttling).
touch "$T/units/gpu-trace.service"
rm -f "$T/calls"
start_guard --setenv PBOS_MEMGUARD_LOW_PCT 100
check "real PSI: both triggers armed" '[[ $(grep -c "trigger on /proc/pressure/memory (some" "$T/log") == 2 ]]'
mkdir -p "$CG/hog"
echo 48M >"$CG/hog/memory.high"
python3 -I -c '
import os, time
os.system("true")
end = time.time() + 25
bufs = []
while time.time() < end:
    b = bytearray(64 << 20)
    for i in range(0, len(b), 4096): b[i] = 1
    bufs.append(b)
    if len(bufs) > 6: bufs.pop(0)
' &
hog=$!
echo "$hog" >"$CG/hog/cgroup.procs"
pids+=("$hog")
for _ in $(seq 1 30); do
  [[ -e "$T/units/gpu-trace.service" ]] || break
  sleep 1
done
check "real PSI: sustained pressure stops gpu-trace" '[[ ! -e $T/units/gpu-trace.service ]]'
kill "$hog" 2>/dev/null || true
stop_guard
check "stop: units stopped by memguard started again" '[[ -e $T/units/gpu-trace.service ]]'
echo "--- log"; cat "$T/log"
exit $fail
