#!/usr/bin/env bash
# The barryglide equivalence harness: barry_glide.py (the rootfs's Python,
# as on the device) against glide-cli over the same cases, for every pack
# given plus a random fuzz pack, then memory and latency.
#
# Usage: run.sh <rootfs> <glide-cli dir> <repo root> <work dir> [pack.zip ...]
#   Shipped packs come from the repo (keyboards/, keyboards-extra/); each
#   zip (e.g. the Languages tab's es.zip) is added as a user pack.
#   SCALE=0.2 for a quick run, SEED=n for other cases.
set -euo pipefail

R="$(cd "${1:?rootfs}" && pwd)"
BIN="$(cd "${2:?glide-cli dir}" && pwd)"
REPO="$(cd "${3:?repo root}" && pwd)"
W="${4:?work dir}"
shift 4
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCALE="${SCALE:-1}"
SEED="${SEED:-1}"

rm -rf "$W"
mkdir -p "$W/shipped" "$W/user" "$W/data" "$W/cache" "$W/bin"
OV="$REPO/sm8550-overlay/usr/share/barry_launcher"
cp -r "$OV/keyboards/en" "$W/shipped/"
cp -r "$OV/keyboards-extra/en-thumbkey" "$W/shipped/"
langs="en,en-thumbkey"
run() {
  bwrap --ro-bind "$R" / --bind "$W" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin --setenv HOME /tmp "$@"
}
for z in "$@"; do
  cp "$z" "$W/pack.zip"
  run python3 -I -c 'import zipfile; zipfile.ZipFile("/build/pack.zip").extractall("/build/user")'
  rm "$W/pack.zip"
done
for d in "$W"/user/*/; do
  [[ -d "$d" ]] && langs="$langs,$(basename "$d")"
done
cp "$HERE"/*.py "$W/bin/"
cp "$BIN/glide-cli" "$BIN/glide-blob" "$W/bin/"
cp "$REPO/sm8550-overlay/usr/lib/barry_launcher/barry_glide.py" "$W/bin/"
printf 'thorr\nbarri\nSteamOSS\n' > "$W/data/personal-words.txt"
printf 'holaa\n' > "$W/data/personal-words-es.txt"
printf 'qqq\n' > "$W/data/personal-words-fuzz.txt"

run python3 -I bin/gen_cases.py --fuzz-pack /build/user/fuzz --seed "$SEED"
langs="$langs,fuzz"
run python3 -I bin/gen_cases.py --dirs /build/shipped:/build/user --langs "$langs" --out /build/cases.jsonl \
  --seed "$SEED" --scale "$SCALE"
DIRS="--dirs /build/shipped:/build/user --data /build/data"
echo "== python"
time run python3 -I bin/ref.py --glide /build/bin $DIRS cases.jsonl > "$W/ref.jsonl"
echo "== c++"
time run bin/glide-cli $DIRS --cache /build/cache cases.jsonl > "$W/cpp.jsonl"
echo "== compare"
run python3 -I bin/compare.py cases.jsonl ref.jsonl cpp.jsonl | tee "$W/compare.txt" || true

echo "== blobs"
for d in "$W"/shipped/en "$W"/user/*/; do
  l="$(basename "$d")"
  [[ -f "$d/words.txt" ]] || continue
  run bin/glide-blob "/build/${d#"$W"/}" "/tmp/$l.blob" $([[ $l == en ]] && echo --english)
done

echo "== memory (each pack alone, after loading and a pass over its cases)"
for l in ${langs//,/ }; do
  grep "\"lang\": \"$l\"" "$W/cases.jsonl" | head -400 > "$W/one.jsonl" || true
  echo "-- $l python"
  run python3 -I bin/ref.py --glide /build/bin $DIRS --load "$l" --rss one.jsonl 2>&1 >/dev/null | grep memory
  echo "-- $l c++ (blob built: first run)"
  rm -rf "$W/cache"/*
  run bin/glide-cli $DIRS --cache /build/cache --load "$l" --rss one.jsonl 2>&1 >/dev/null | grep memory
  echo "-- $l c++ (blob mapped from the cache)"
  run bin/glide-cli $DIRS --cache /build/cache --load "$l" --rss one.jsonl 2>&1 >/dev/null | grep memory
done
ls -l "$W/cache"

echo "== latency"
printf '%s\n' "$(grep -m1 '"lang": "en", "op": "glide"' "$W/cases.jsonl")" > "$W/long.jsonl"
run python3 -I - <<'EOF'
import json
c = json.loads(open("/build/long.jsonl").read())
print("long glide case:", len(c["path"]), "points")
EOF
grep -m300 '"op": "glide"' "$W/cases.jsonl" > "$W/glides.jsonl"
grep -m600 -E '"op": "(correct|suggest)"' "$W/cases.jsonl" > "$W/typed.jsonl"
for f in long glides typed; do
  echo "-- $f python"
  run python3 -I bin/ref.py --glide /build/bin $DIRS --repeat 20 $f.jsonl 2>&1 >/dev/null | grep time
  echo "-- $f c++"
  run bin/glide-cli $DIRS --cache /build/cache --repeat 20 $f.jsonl 2>&1 >/dev/null | grep time
done
