#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/threshold-smoke}
YAML=${1:-samples/route-examples.yaml}

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

if [ ! -x "$BUILD_DIR/eventnet_agent" ] || [ ! -x "$BUILD_DIR/eventnetd" ]; then
  printf '%s\n' 'eventnet_agent/eventnetd not found. Run sh scripts/vm-build-cc.sh first.' >&2
  exit 1
fi

STREAM_FILE="$OUT_DIR/stream.jsonl"
: > "$STREAM_FILE"
make_round() {
  direct_mode=$1
  direct_count=$2
  round_file=$3
  "$BUILD_DIR/eventnet_agent" --path path-legacy-single-route --target 203.0.113.9 \
    --count "$direct_count" --interval-ms 0 --simulate "$direct_mode" "$([ "$direct_mode" = 0 ] && printf '100' || printf '0')" --output "$OUT_DIR/direct.jsonl"
  "$BUILD_DIR/eventnet_agent" --path path-hub-node-routes --target 203.0.113.13 \
    --count 1 --interval-ms 0 --simulate 10 0 --output "$OUT_DIR/hub.jsonl"
  tail -n 1 "$OUT_DIR/direct.jsonl" > "$round_file"
  cat "$round_file" "$OUT_DIR/hub.jsonl" >> "$STREAM_FILE"
}

make_round 10 1 "$OUT_DIR/healthy.jsonl"
make_round 0 1 "$OUT_DIR/failure-1.jsonl"
make_round 0 3 "$OUT_DIR/failure-3.jsonl"
make_round 10 1 "$OUT_DIR/recovery-1.jsonl"
make_round 10 2 "$OUT_DIR/recovery-2.jsonl"

STATE_FILE="$OUT_DIR/state.tsv"
"$BUILD_DIR/eventnetd" "$YAML" --intent intent-vlan-direct --telemetry-stdin \
  --backend mock --max-age-ms 60000 --batch-size 2 --count 5 --state-file "$STATE_FILE" < "$STREAM_FILE" > "$OUT_DIR/result.txt"

[ "$(grep -c 'selected_path: path-legacy-single-route' "$OUT_DIR/result.txt")" -eq 3 ]
[ "$(grep -c 'selected_path: path-hub-node-routes' "$OUT_DIR/result.txt")" -eq 2 ]
awk '/selected_path:/{print $2}' "$OUT_DIR/result.txt" > "$OUT_DIR/selected-paths.txt"
sed -n '1p' "$OUT_DIR/selected-paths.txt" | grep -q '^path-legacy-single-route$'
sed -n '2p' "$OUT_DIR/selected-paths.txt" | grep -q '^path-legacy-single-route$'
sed -n '3p' "$OUT_DIR/selected-paths.txt" | grep -q '^path-hub-node-routes$'
sed -n '4p' "$OUT_DIR/selected-paths.txt" | grep -q '^path-hub-node-routes$'
sed -n '5p' "$OUT_DIR/selected-paths.txt" | grep -q '^path-legacy-single-route$'

printf '%s\n' 'Threshold smoke passed: Agent streaks controlled eventnetd fallback and recovery.'
