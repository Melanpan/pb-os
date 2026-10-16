#!/bin/bash
# Builds the Water skin's shader (water.frag) into the .qsb file
# WaterPad.qml loads, with Qt's qsb (qt6-shadertools; in the image's rootfs
# at /usr/lib/qt6/bin/qsb). Run again after changing it.
set -e
cd "$(dirname "$0")"
QSB=${QSB:-/usr/lib/qt6/bin/qsb}
"$QSB" --glsl "100es,120,150" --hlsl 50 --msl 12 -o water.frag.qsb water.frag
