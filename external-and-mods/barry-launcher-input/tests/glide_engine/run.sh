#!/usr/bin/env bash
# The keyboards' glide typing, suggestions and autocorrect in Barry.Input
# (GlideEngine) against barry_launcher_shelld's HTTP answers (/glide,
# /suggest, /type), over the same cases (compare.py), in a SteamOS rootfs
# (read-only) under bwrap, as root in the build VM. shelld is the worktree's,
# its request handler served on a test port (no session around it); a stub
# barry_launcher_keyd answers "BEFORE n" with the field's text each case
# sets.
#
# Usage: sudo run.sh <rootfs> <repo root> <work dir> [pack.zip ...]
#   Shipped packs (en, en-thumbkey) come from the repo; each zip (the
#   Languages tab's es.zip) is added as a user pack, and a random fuzz pack
#   (glide/tests/gen_cases.py's). SCALE=0.2 for a quick run, SEED=n for other
#   cases.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
REPO="$(cd "${2:?repo root}" && pwd)"
W="${3:?work dir}"
shift 3
SCALE="${SCALE:-1}"
SEED="${SEED:-1}"

rm -rf "$W"
mkdir -p "$W/src" "$W/shipped" "$W/user" "$W/bin" "$W/lib"
run() {
  bwrap --ro-bind "$R" / --bind "$W" /t --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --unshare-net --die-with-parent --chdir /t --setenv PATH /usr/bin --setenv HOME /tmp "$@"
}

echo "== build (Barry.Input's GlideEngine, as the keyboards build it)"
cp -r "$SRC"/. "$W/src/"
run cmake -S /t/src -B /t/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARRY_INPUT_GLIDE_TEST=ON >/dev/null
run cmake --build /t/out --target glide-engine-test >/dev/null
cp "$W/out/glide-engine-test" "$W/bin/"

OV="$REPO/sm8550-overlay/usr/share/barry_launcher"
cp -r "$OV/keyboards/en" "$W/shipped/"
cp -r "$OV/keyboards-extra/en-thumbkey" "$W/shipped/"
langs="en,en-thumbkey"
for z in "$@"; do
  cp "$z" "$W/pack.zip"
  run python3 -I -c 'import zipfile; zipfile.ZipFile("/t/pack.zip").extractall("/t/user")'
  rm "$W/pack.zip"
done
for d in "$W"/user/*/; do
  [[ -d "$d" ]] && langs="$langs,$(basename "$d")"
done
cp -a "$REPO/sm8550-overlay/usr/lib/barry_launcher/." "$W/lib/"
cp "$SRC/glide/tests/gen_cases.py" "$HERE/compare.py" "$W/bin/"
run python3 -I bin/gen_cases.py --fuzz-pack /t/user/fuzz --seed "$SEED"
langs="$langs,fuzz"
run python3 -I bin/gen_cases.py --dirs /t/shipped:/t/user --langs "$langs" --out /t/cases.jsonl \
  --seed "$SEED" --scale "$SCALE"

echo "== compare"
run python3 -I bin/compare.py /t/lib/barry_launcher_shelld /t/bin/glide-engine-test /t/cases.jsonl "$langs"
