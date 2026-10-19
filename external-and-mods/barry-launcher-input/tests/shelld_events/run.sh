#!/usr/bin/env bash
# barry_launcher_shelld's event stream test (shelld_events.py), in a SteamOS
# rootfs (read-only) under bwrap, as root in the build VM: Xvfb from the rootfs,
# stub xdotool, firefox, qml6 (qml6_stub.py), flatpak and gamescopectl, no
# network. Apps in the background: stub systemd-run (a real cgroup v2 scope of
# its own under $CG, which shelld freezes for real) and pactl (sound streams as
# /t/pactl-sink-inputs.json says, events as appended to /t/pactl-events); the
# memory pressure trigger is a FIFO (/t/psi).
#
# Usage: sudo run.sh <rootfs> <sm8550-overlay> <old shelld> [--cpu-only]
#   <old shelld>: the one before the event stream, for the idle CPU and GET
#   comparison (git show f688c38:sm8550-overlay/usr/lib/barry_launcher/barry_launcher_shelld)
# IDLE_S and CPU_S (seconds) shorten or lengthen the idle checks.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"; OV="$(cd "${2:?sm8550-overlay}" && pwd)"; OLD="$(realpath "${3:?old shelld}")"
shift 3
work="$(mktemp -d)"
CG=/sys/fs/cgroup/barry-shelld-test.slice
cleanup() {
  # The scopes' cgroups: their processes ended (cgroup.kill), then removed.
  if [[ -d "$CG" ]]; then
    for d in "$CG"/*/; do
      [[ -d "$d" ]] || continue
      echo 0 >"$d/cgroup.freeze" 2>/dev/null || true
      echo 1 >"$d/cgroup.kill" 2>/dev/null || true
      for _ in $(seq 50); do rmdir "$d" 2>/dev/null && break; sleep 0.1; done
    done
    rmdir "$CG" 2>/dev/null || echo "left $CG behind" >&2
  fi
  rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$CG"
mkdir -p "$work/stubs" "$work/lib"
cp "$HERE/shelld_events.py" "$work/"
cp -a "$OV/usr/lib/barry_launcher/." "$work/lib/"
cp "$OLD" "$work/lib/barry_launcher_shelld_old"
printf '#!/bin/sh\nexit 0\n' > "$work/stubs/xdotool"
printf '#!/bin/sh\nexit 0\n' > "$work/stubs/gamescopectl"
printf '#!/bin/sh\nexit 1\n' > "$work/stubs/flatpak"
# An app runs until it is ended; its arguments show in ps. qml6 as a tool
# (the trackpad, the Deck controls) also plays ToolStage.qml's part on the
# event stream (qml6_stub.py).
printf '#!/bin/sh\nwhile :; do sleep 1000; done\n' > "$work/stubs/firefox"
cp "$HERE/qml6_stub.py" "$work/stubs/qml6"
# systemd-run --user --scope [options] --unit=NAME [-p X]... -- command: as
# systemd's, it execs the command in place, in a cgroup of its own.
cat >"$work/stubs/systemd-run" <<EOF
#!/bin/sh
unit=
while [ \$# -gt 0 ]; do
  case "\$1" in
    --unit=*) unit="\${1#--unit=}" ;;
    -p) shift ;;
    --) shift; break ;;
  esac
  shift
done
mkdir -p "$CG/\$unit.scope" && echo \$\$ >"$CG/\$unit.scope/cgroup.procs" || exit 1
echo "\$unit \$*" >>/t/systemd-run.log
exec "\$@"
EOF
cat >"$work/stubs/pactl" <<'EOF'
#!/bin/sh
case "$*" in
  *subscribe*) exec tail -n0 -F /t/pactl-events 2>/dev/null ;;
  *sink-inputs*) cat /t/pactl-sink-inputs.json 2>/dev/null || echo '[]' ;;
  *) echo '[]' ;;
esac
EOF
chmod +x "$work/stubs/"*
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --bind "$work" /t \
  --ro-bind /sys /sys --bind /sys/fs/cgroup /sys/fs/cgroup \
  --ro-bind "$OV/usr/share/barry_launcher" /usr/share/barry_launcher \
  --unshare-net --unshare-pid --die-with-parent --chdir /t \
  --setenv PATH /t/stubs:/usr/bin --setenv IDLE_S "${IDLE_S:-30}" --setenv CPU_S "${CPU_S:-60}" \
  python3 /t/shelld_events.py /t/lib/barry_launcher_shelld /t/lib/barry_launcher_shelld_old "$@"
