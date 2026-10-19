#!/usr/bin/env bash
# Off-device check of the audio-dp module (root, in the Mini's Linux VM):
# fake DRM connectors and device tree bound over /sys, a stub policy script
# that logs each run, and DRM uevents sent on the kernel's uevent group.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd binary>
set -euo pipefail
R="$(cd "${1:?rootfs}" && pwd)"
BIN="$(realpath "${2:?pbos-platformd}")"
T="$(mktemp -d)"
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$T"' EXIT
mkdir -p "$T/sys/class/drm/card0-DP-1" "$T/sys/class/drm/card0-DSI-1" "$T/sys/firmware/devicetree/base" "$T/steamos"
ln -s sys/class/drm "$T/drm"
ln -s sys/firmware/devicetree/base "$T/dt"
cp "$BIN" "$T/pbos-platformd"
echo disconnected >"$T/drm/card0-DP-1/status"
echo connected >"$T/drm/card0-DSI-1/status"
printf 'ayn,thor\0qcom,sm8550\0' >"$T/dt/compatible"
cat >"$T/steamos/sm8550-audio-pipewire" <<STUB
#!/bin/bash
echo "run \$(cat /sys/class/drm/card0-DP-1/status)" >>"$T/runs"
echo "sm8550-audio-pipewire: stub ran" >&2
sleep "\${STUB_SLEEP:-0}"
STUB
chmod +x "$T/steamos/sm8550-audio-pipewire"

# A uevent as the kernel sends it: "ACTION@DEVPATH\0KEY=value\0...".
send() {
  python3 -I - "$1" "$2" <<'PY'
import socket, sys
sub, act = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 15)
s.bind((0, 0))
msg = f"{act}@/devices/x\0ACTION={act}\0DEVPATH=/devices/x\0SUBSYSTEM={sub}\0HOTPLUG=1\0".encode()
s.sendto(msg, (0, 1))
PY
}
runs() { [[ -f "$T/runs" ]] && wc -l <"$T/runs" | tr -d ' ' || echo 0; }
fail=0
expect() {
  local want="$1" what="$2"
  if [[ "$(runs)" == "$want" ]]; then echo "PASS $what (runs=$want)"; else echo "FAIL $what (runs=$(runs), want $want)"; fail=1; fi
}

bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$T" "$T" \
  --bind "$T/sys" /sys --bind "$T/steamos" /usr/lib/steamos --die-with-parent \
  --setenv PBOS_AUDIO_DP_LAST off --setenv STUB_SLEEP 1 \
  "$T/pbos-platformd" --user --only audio-dp 2>"$T/log" &
pid=$!
sleep 1
expect 0 "start, no change"

send block add; sleep 1
expect 0 "non-DRM uevent"

send drm change; sleep 1
expect 0 "DRM uevent, nothing changed"

echo connected >"$T/drm/card0-DP-1/status"
for _ in 1 2 3 4 5; do send drm change; sleep 0.1; done
sleep 1
expect 1 "plug (burst of 5) -> one run"
grep -q "run connected" "$T/runs" && echo "PASS run saw connected" || { echo "FAIL run status"; fail=1; }

# Pulled while the policy runs (stub sleeps 1 s): one more run after it.
echo disconnected >"$T/drm/card0-DP-1/status"; send drm change; sleep 0.7
echo connected >"$T/drm/card0-DP-1/status"; send drm change; sleep 0.7
expect 2 "pull during a run"
sleep 2.5
expect 3 "re-plug while running -> run after it"

echo disconnected >"$T/drm/card0-DP-1/status"; send drm change; sleep 2
expect 4 "pull"
kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true

# Started with a stale LAST: applies at once.
rm -f "$T/runs"
echo connected >"$T/drm/card0-DP-1/status"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$T" "$T" \
  --bind "$T/sys" /sys --bind "$T/steamos" /usr/lib/steamos --die-with-parent --setenv PBOS_AUDIO_DP_LAST off \
  "$T/pbos-platformd" --user --only audio-dp 2>>"$T/log" &
pid=$!
sleep 1
expect 1 "flip before start"
kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
echo "--- log"; cat "$T/log"
exit $fail
