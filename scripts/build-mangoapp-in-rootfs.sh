#!/usr/bin/env bash
# Build PB-OS's mangoapp (Steam's Performance Overlay) inside the Frame rootfs
# (glibc 2.39), like build-gamescope-in-rootfs.sh.
#
# Valve's stock ARM mangoapp reads GPU load from its own fdinfo, so the
# overlay shows GPU 0 % on every PB-OS device. Upstream MangoHud reads the
# focused game's fdinfo. The source is upstream at the commit in
# external-and-mods/mangoapp/REF plus the patches next to it.
#
# The rootfs is never written: the build runs in a throwaway overlay of it.
# Meson comes from a release tarball (the Frame has 1.4, upstream needs 1.7).
#
# Usage: build-mangoapp-in-rootfs.sh <rootfs> [build-dir]
# Output: <build-dir>/mangoapp (stripped) and <build-dir>/stamp
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODS="${ROOT}/external-and-mods/mangoapp"
R="$(cd "${1:?rootfs}" && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
BUILD="${2:-${WORKDIR}/mangoapp-build}"
CLONE="${WORKDIR}/mangohud-src"
OVL="${WORKDIR}/mangoapp-ovl"
MESON_VER=1.9.1
MESON="${WORKDIR}/meson-${MESON_VER}"
REPO=https://github.com/flightlessmango/MangoHud.git
REF="$(tr -d '[:space:]' < "${MODS}/REF")"

log() { printf '==> [mangoapp] %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/gcc" && -x "$R/usr/bin/ninja" ]] || die "rootfs needs gcc+ninja"

# Same source + patches + rootfs → reuse the last build.
stamp="$(cat "${MODS}/REF" "${MODS}"/*.patch "${BASH_SOURCE[0]}" | sha256sum | cut -c1-16) $(cat "$R/usr/lib/os-release" | grep '^BUILD_ID=')"
if [[ -x "$BUILD/mangoapp" && "$(cat "$BUILD/stamp" 2>/dev/null)" == "$stamp" ]]; then
  log "up to date: $BUILD/mangoapp"
  exit 0
fi

g=(git -c safe.directory='*' -C "$CLONE")
if [[ ! -d "$CLONE/.git" ]]; then
  log "cloning $REPO"
  git clone -q "$REPO" "$CLONE"
fi
"${g[@]}" cat-file -e "${REF}^{commit}" 2>/dev/null || "${g[@]}" fetch -q origin
"${g[@]}" -c advice.detachedHead=false checkout -q -f "$REF"
"${g[@]}" clean -qfdx -e subprojects/packagecache
for p in "${MODS}"/*.patch; do
  log "patch $(basename "$p")"
  "${g[@]}" apply "$p"
done

if [[ ! -f "$MESON/meson.py" ]]; then
  log "meson ${MESON_VER}"
  curl -sfL "https://github.com/mesonbuild/meson/releases/download/${MESON_VER}/meson-${MESON_VER}.tar.gz" \
    | tar -xz -C "$WORKDIR"
fi

cleanup() {
  mountpoint -q "$OVL/root" && umount "$OVL/root"
  rm -rf "$OVL"
}
trap cleanup EXIT
cleanup
mkdir -p "$OVL/upper" "$OVL/wk" "$OVL/root"
mount -t overlay overlay -o "lowerdir=$R,upperdir=$OVL/upper,workdir=$OVL/wk" "$OVL/root"
# Meson fetches its wraps (imgui, implot, …); the Frame has no resolv.conf.
cp /etc/resolv.conf "$OVL/root/etc/resolv.conf"
mkdir -p "$OVL/root/mnt/src" "$OVL/root/mnt/build" "$OVL/root/opt/meson"

rm -rf "$BUILD"
mkdir -p "$BUILD"
run() {
  bwrap --bind "$OVL/root" / \
    --bind "$CLONE" /mnt/src --bind "$BUILD" /mnt/build --bind "$MESON" /opt/meson \
    --dev /dev --proc /proc --tmpfs /run --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /mnt/src \
    --setenv PATH /usr/bin --setenv HOME /tmp \
    "$@"
}
log "meson setup (Frame rootfs, upstream $REF)"
run python3 /opt/meson/meson.py setup /mnt/build/meson --buildtype=release \
  -Dmangoapp=true -Dmangohudctl=false -Dwith_fex=true -Dwith_nvml=disabled \
  -Dwith_xnvctrl=disabled -Dtests=disabled -Dmangoplot=disabled -Dinclude_doc=false \
  -Dwith_server=false -Dwith_mangohud_next=false
log "ninja"
run ninja -C /mnt/build/meson src/mangoapp
run strip -o /mnt/build/mangoapp /mnt/build/meson/src/mangoapp
[[ -x "$BUILD/mangoapp" ]] || die "no mangoapp binary"
if strings "$BUILD/mangoapp" | grep -q 'GLIBC_2\.4[0-9]'; then
  die "mangoapp links against a newer glibc than the Frame"
fi
printf '%s\n' "$stamp" > "$BUILD/stamp"
log "OK: $BUILD/mangoapp"
