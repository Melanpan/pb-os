#!/usr/bin/env bash
# shell/ToolStage.qml's slides with Barry.Input's ShellState (tool_stage.py),
# offscreen, in a SteamOS rootfs (read-only) under bwrap, as root in the
# build VM, against a stand-in shelld socket; no network.
#
# Usage: sudo run.sh <rootfs> <sm8550-overlay> <Barry.Input module dir>
#   <Barry.Input module dir>: build.sh's output (.../qml/Barry/Input)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"; OV="$(cd "${2:?sm8550-overlay}" && pwd)"; MOD="$(cd "${3:?module dir}" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/qml/Barry" "$work/view"
cp -a "$MOD" "$work/qml/Barry/Input"
cp "$HERE/ToolStageTest.qml" "$OV/usr/share/barry_launcher/shell/ToolStage.qml" "$work/view/"
cp "$HERE/tool_stage.py" "$work/"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --bind "$work" /t \
  --unshare-net --unshare-pid --die-with-parent --chdir /t \
  --setenv PATH /usr/bin --setenv HOME /tmp --setenv QML_IMPORT_PATH /t/qml \
  python3 /t/tool_stage.py /t/view
