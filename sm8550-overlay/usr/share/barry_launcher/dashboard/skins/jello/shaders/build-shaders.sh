#!/bin/bash
# Builds the Jello skin's shaders (jelly.vert, jelly.frag) into the .qsb
# files JellyPad.qml loads, with Qt's qsb (qt6-shadertools; in the image's
# rootfs at /usr/lib/qt6/bin/qsb). Run again after changing either one.
set -e
cd "$(dirname "$0")"
QSB=${QSB:-/usr/lib/qt6/bin/qsb}
for f in jelly.vert jelly.frag; do
    "$QSB" --glsl "100es,120,150" --hlsl 50 --msl 12 -o "$f.qsb" "$f"
done
