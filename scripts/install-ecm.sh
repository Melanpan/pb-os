#!/usr/bin/env bash
# Install KDE Extra CMake Modules (build-time cmake files only) from KDE's
# own release archive, which keeps every version (distro mirrors drop old
# packages). Configured with the rootfs cmake via bubblewrap (rootfs read-only).
#
# Usage: install-ecm.sh <rootfs>            into <rootfs>/usr/share/ECM, unless
#                                           it already has ECM >= ECM_MIN
#        install-ecm.sh <rootfs> <destdir>  always into <destdir>/usr/share/ECM
# Env:   STEAMOS_WORK  work dir, default /work (tarball cached in $WORK/cache/ecm)
set -euo pipefail

R="$(cd "${1:?rootfs}" && pwd)"
DEST="${2:-}"
CACHE="${STEAMOS_WORK:-/work}/cache/ecm"

# ECM 6.30.0 needs cmake >= 3.29 (the Frame base has 3.29.2): check before bumping.
ECM_VER=6.30.0
ECM_URL="https://download.kde.org/stable/frameworks/${ECM_VER%.*}/extra-cmake-modules-${ECM_VER}.tar.xz"
ECM_SHA=22c9f7ff930dae7329faebf2942d2424f4a298c43bb3f11e217be6b12f227f6e
# Plasma 6.2.5 needs >= 6.5; the Frame base ships 6.1.0 or none.
ECM_MIN=6.5.0

log() { printf '==> [ecm] %s\n' "$*"; }
die() { printf 'ERROR: [ecm] %s\n' "$*" >&2; exit 1; }

if [[ -z "$DEST" ]]; then
  v="$(sed -n 's/^set(PACKAGE_VERSION "\([0-9.]*\)").*/\1/p' \
    "$R/usr/share/ECM/cmake/ECMConfigVersion.cmake" 2>/dev/null | head -1)"
  if [[ -n "$v" && -f "$R/usr/share/ECM/cmake/ECMConfig.cmake" \
        && "$(printf '%s\n%s\n' "$v" "$ECM_MIN" | sort -V | head -1)" == "$ECM_MIN" ]]; then
    exit 0
  fi
  DEST="$R"
fi

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/cmake" && -x "$R/usr/bin/ninja" ]] || die "rootfs needs cmake and ninja"

mkdir -p "$CACHE"
TGZ="$CACHE/extra-cmake-modules-${ECM_VER}.tar.xz"
if [[ ! -f "$TGZ" ]] || [[ "$(sha256sum "$TGZ" | cut -d' ' -f1)" != "$ECM_SHA" ]]; then
  log "downloading $(basename "$TGZ")"
  curl -fsSL --max-time 300 -o "$TGZ.part" "$ECM_URL" && mv "$TGZ.part" "$TGZ"
fi
[[ "$(sha256sum "$TGZ" | cut -d' ' -f1)" == "$ECM_SHA" ]] || die "checksum mismatch: $TGZ"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/build" "$tmp/stage"
tar -xJf "$TGZ" -C "$tmp"
run() {
  bwrap --ro-bind "$R" / \
    --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run \
    --ro-bind "$tmp/extra-cmake-modules-${ECM_VER}" /tmp/src \
    --bind "$tmp/build" /tmp/build \
    --bind "$tmp/stage" /tmp/stage \
    --unshare-pid --die-with-parent --chdir /tmp \
    --setenv PATH /usr/bin --setenv HOME /tmp \
    "$@"
}
log "installing ECM ${ECM_VER} into ${DEST}"
run /usr/bin/cmake -S /tmp/src -B /tmp/build -G Ninja \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_DOC=OFF \
  -DBUILD_TESTING=OFF >/dev/null
run env DESTDIR=/tmp/stage /usr/bin/cmake --install /tmp/build >/dev/null
[[ -f "$tmp/stage/usr/share/ECM/cmake/ECMConfig.cmake" ]] || die "install output incomplete"
rm -rf "$DEST/usr/share/ECM"
mkdir -p "$DEST/usr/share"
cp -a "$tmp/stage/usr/share/ECM" "$DEST/usr/share/"
