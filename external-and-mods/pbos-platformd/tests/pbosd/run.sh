#!/usr/bin/env bash
# Off-device equivalence test of the pbosd module against the Python pbosd
# (see scenario.py). Root in the build VM; the rootfs is bound read-only, and
# /sys, /etc, /run, /var/lib, /home and /usr/share are throwaway fakes;
# systemctl, runuser and pgrep are stubs that log their argv.
#
# No power key is ever made: the key cases write input events to FIFOs that
# pose as /dev/input/event*, and the uinput cases make only an F13-F15
# keyboard and a touchscreen.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <pbosd (Python)> <outdir> [CASE...]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?pbosd}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
b="$(mktemp -d)"
trap 'rm -rf "$b"' EXIT
cp "$HERE"/scenario.py "$HERE"/wlog.c "$HERE"/procs.c "$b/"
cp "$BIN" "$b/pbos-platformd"
cp "$PY" "$b/pbosd.py"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$b" /tmp/b --chdir /tmp/b --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -shared -fPIC -o wlog.so wlog.c -ldl && gcc -O2 -pthread -o procs procs.c'
for n in reaper game gamescope-wl steam other; do cp "$b/procs" "$b/$n"; done
cat >"$b/stub" <<'EOS'
#!/bin/sh
echo "$(basename "$0") $* | HOME=$HOME USER=$USER PATH=$PATH DISPLAY=$DISPLAY XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS" >>/tmp/ctl/cmds
EOS
cat >"$b/pgrep" <<'EOS'
#!/bin/sh
echo "pgrep $*" >>/tmp/ctl/cmds
[ -e /tmp/ctl/desktop ]
EOS
chmod +x "$b/stub" "$b/pgrep"
cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || read -ra cases <<<"$(python3 "$b/scenario.py" --list)"
for c in "${cases[@]}"; do
  for impl in py cpp; do
    t="$(mktemp -d)"
    mkdir -p "$t"/{sys/class/input,sys/devices/virtual,run,varlib,home,ctl}
    cp -a "$R/etc" "$t/etc"
    args=(--ro-bind "$R" / --proc /proc --bind "$t/sys" /sys --bind "$t/etc" /etc --bind "$t/run" /run
          --bind "$t/varlib" /var/lib --bind "$t/home" /home --tmpfs /usr/share
          --tmpfs /tmp --bind "$t/ctl" /tmp/ctl --bind "$b" /tmp/t --bind "$OUT" /tmp/out
          --ro-bind "$b/stub" /usr/bin/systemctl --ro-bind "$b/stub" /usr/bin/runuser --ro-bind "$b/pgrep" /usr/bin/pgrep
          --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin)
    case "$c" in
      touch|hotplug) args+=(--dev-bind /dev /dev --ro-bind /sys/class/input /sys/class/input
                            --ro-bind /sys/devices/virtual /sys/devices/virtual --unshare-pid) ;;
      games-cn) args+=(--dev /dev --tmpfs /dev/input) ;;  # the proc connector needs the first pid namespace
      *) args+=(--dev /dev --tmpfs /dev/input --unshare-pid) ;;
    esac
    echo "== $c ($impl)"
    if ! bwrap "${args[@]}" python3 /tmp/t/scenario.py "$impl" "$c"; then
      echo "!! $c ($impl) failed"
    fi
    [[ -n "${KEEP:-}" ]] && echo "kept $t" || rm -rf "$t"
  done
done
python3 "$HERE/compare.py" "$OUT" "${cases[@]}"
