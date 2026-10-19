#!/usr/bin/env bash
# Build the motion sensor stack inside a SteamOS rootfs (as pbos-qam/build.sh
# does), read-only: the rootfs has gcc/g++, meson, ninja and json-c.
#   hexagonrpcd     serves the ADSP sensors process its files (registry,
#                   sensor config) over FastRPC; linux-msm/hexagonrpc at
#                   HEXAGONRPC_REF with the patches in hexagonrpc/
#   sscregistrygen  makes the sensor registry from the stock JSON config
#   pbos-motiond    the sensors as a uinput motion device (daemon/; libsystemd)
# hexagonrpc's source is fetched into CACHE (default: next to this script).
#
# Usage: build.sh <rootfs> <output dir>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output dir}"
CACHE="${PBOS_MOTION_CACHE:-$HERE/.cache}"
HEXAGONRPC_URL=https://github.com/linux-msm/hexagonrpc.git
HEXAGONRPC_REF=dd9ac70c026e1bad93e8cffa3801255b8ceb551e

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
for f in usr/bin/g++ usr/bin/meson usr/bin/ninja usr/include/json-c/json.h usr/include/linux/qrtr.h \
  usr/include/systemd/sd-bus.h; do
  [[ -e "$R/$f" ]] || { echo "rootfs needs /$f" >&2; exit 1; }
done

src="$CACHE/hexagonrpc"
if [[ ! -d "$src/.git" ]]; then
  mkdir -p "$CACHE"
  git clone -q "$HEXAGONRPC_URL" "$src"
fi
git -C "$src" cat-file -e "${HEXAGONRPC_REF}^{commit}" 2>/dev/null || git -C "$src" fetch -q origin

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git -C "$src" archive "$HEXAGONRPC_REF" | tar -C "$work" --one-top-level=hexagonrpc -x
for p in "$HERE"/hexagonrpc/*.patch; do
  patch -d "$work/hexagonrpc" -p1 -s --no-backup-if-mismatch <"$p" || { echo "patch failed: $p" >&2; exit 1; }
done
# libhexagonrpc is a shared_library() upstream; as library() it follows
# --default-library=static, so it is built into hexagonrpcd.
sed -i "s/^libhexagonrpc = shared_library(/libhexagonrpc = library(/" \
  "$work/hexagonrpc/libhexagonrpc/meson.build"
grep -q "^libhexagonrpc = library(" "$work/hexagonrpc/libhexagonrpc/meson.build" \
  || { echo "cannot make libhexagonrpc static" >&2; exit 1; }
cp "$HERE"/daemon/*.cpp "$work/"
run() {
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin "$@"
}
run meson setup --buildtype=release --default-library=static -Dhexagonrpcd_verbose=false \
  hexagonrpc out >/dev/null
run ninja -C out hexagonrpcd/hexagonrpcd tools/sscregistrygen >/dev/null
run g++ -std=c++20 -O2 -g0 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -Wall -Wextra -Werror -fno-plt -o pbos-motiond pbos-motiond.cpp -lsystemd
run strip out/hexagonrpcd/hexagonrpcd out/tools/sscregistrygen pbos-motiond
install -D -m0755 "$work/out/hexagonrpcd/hexagonrpcd" "$OUT/hexagonrpcd"
install -D -m0755 "$work/out/tools/sscregistrygen" "$OUT/sscregistrygen"
install -D -m0755 "$work/pbos-motiond" "$OUT/pbos-motiond"
echo "built $OUT/{hexagonrpcd,sscregistrygen,pbos-motiond}"
