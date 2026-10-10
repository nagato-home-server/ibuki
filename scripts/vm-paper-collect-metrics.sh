#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML=${1:-samples/linux-vm-netns.yaml}
[ "$#" -eq 0 ] || shift
REPEAT=${REPEAT:-5}
BUILD_DIR=${BUILD_DIR:-build-linux-cc}
OUT_DIR=${OUT_DIR:-out/paper-live-$(date +%Y%m%d-%H%M%S)}

cd "$ROOT_DIR"
printf 'Live measurement: repeats=%s output=%s\n' "$REPEAT" "$OUT_DIR"
python3 scripts/paper-runtime.py evaluate --yaml "$YAML" --repeat "$REPEAT" \
  --build-dir "$BUILD_DIR" --output "$OUT_DIR" "$@"
python3 scripts/generate-paper-graphs.py --metrics-csv "$OUT_DIR/metrics.csv" --out-dir "$OUT_DIR/figures"
if [ -n "${OUT_FILE:-}" ]; then
  mkdir -p "$(dirname -- "$OUT_FILE")"
  cp "$OUT_DIR/metrics.csv" "$OUT_FILE"
fi
printf 'Measurement and figures written: %s\n' "$OUT_DIR"
