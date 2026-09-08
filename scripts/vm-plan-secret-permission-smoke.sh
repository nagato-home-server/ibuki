#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/plan-secret-permission}"

cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnet_yaml_demo" ]; then sh "$ROOT_DIR/scripts/vm-build-cc.sh"; fi
mkdir -p "$OUT_DIR"
conf="$OUT_DIR/swanctl.conf"
sentinel="$OUT_DIR/sentinel"
link="$OUT_DIR/swanctl-link.conf"
rm -f "$conf" "$link"
printf '%s\n' 'do-not-overwrite' > "$sentinel"

"$BUILD_DIR/eventnet_yaml_demo" samples/ipsec-routes.yaml --intent intent-a-b --conf "$conf" > "$OUT_DIR/generate.log"
mode=$(stat -c '%a' "$conf")
[ "$mode" = "600" ]

ln -s "$sentinel" "$link"
if "$BUILD_DIR/eventnet_yaml_demo" samples/ipsec-routes.yaml --intent intent-a-b --conf "$link" > "$OUT_DIR/symlink.log" 2>&1; then
  printf '%s\n' 'symlink target unexpectedly accepted' >&2
  exit 1
fi
grep -q 'failed to write swanctl conf' "$OUT_DIR/symlink.log"
grep -q 'do-not-overwrite' "$sentinel"

printf 'Plan secret permission smoke passed: mode=0600 and symlink target was protected.\n'
