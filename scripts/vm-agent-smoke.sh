#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/agent-smoke}"
YAML="${1:-$ROOT_DIR/samples/linux-vm-netns.yaml}"
INTENT_ID="${INTENT_ID:-intent-a-b}"

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

if [ ! -x "$BUILD_DIR/eventnet_agent" ]; then
  printf 'eventnet_agent not found. Run sh scripts/vm-build-cc.sh first.\n' >&2
  exit 1
fi

"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 3 --interval-ms 1 --simulate 12.5 0.0 --output "$OUT_DIR/healthy.jsonl"
grep -q '"schema":"ibuki.telemetry.path_health.v1"' "$OUT_DIR/healthy.jsonl"
grep -q '"state":"healthy"' "$OUT_DIR/healthy.jsonl"

rm -f "$OUT_DIR/append.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 \
  --count 1 --simulate 12.5 0.0 --output "$OUT_DIR/append.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 \
  --count 1 --simulate 12.5 0.0 --append --output "$OUT_DIR/append.jsonl"
[ "$(wc -l < "$OUT_DIR/append.jsonl" | tr -d ' ')" -eq 2 ]

if [ "$(uname -s)" = "Linux" ]; then
  secure_target="$OUT_DIR/secure-target.jsonl"
  secure_link="$OUT_DIR/secure-link.jsonl"
  printf '%s\n' sentinel > "$secure_target"
  ln -s "$(basename "$secure_target")" "$secure_link"
  set +e
  "$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 \
    --count 1 --simulate 12.5 0.0 --output "$secure_link" >/dev/null 2>&1
  symlink_status=$?
  chmod 0666 "$secure_target"
  "$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 \
    --count 1 --simulate 12.5 0.0 --output "$secure_target" >/dev/null 2>&1
  writable_status=$?
  set -e
  chmod 0600 "$secure_target"
  rm -f "$secure_link" "$secure_target"
  if [ "$symlink_status" -eq 0 ] || [ "$writable_status" -eq 0 ]; then
    printf '%s\n' 'Agent smoke failed: unsafe output file was accepted.' >&2
    exit 1
  fi
fi

"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 1 --simulate 0 100.0 --output "$OUT_DIR/failed.jsonl"
grep -q '"state":"failed"' "$OUT_DIR/failed.jsonl"

"$BUILD_DIR/eventnet_agent" --yaml "$YAML" --intent "$INTENT_ID" \
  --count 1 --interval-ms 0 --simulate 12.5 0.0 --output "$OUT_DIR/yaml.jsonl"
yaml_probe_count=$(wc -l < "$OUT_DIR/yaml.jsonl" | tr -d ' ')
if [ "$yaml_probe_count" -le 0 ]; then
  printf '%s\n' 'YAML Agent produced no telemetry records.' >&2
  exit 1
fi
case "$YAML" in
  *linux-vm-netns.yaml)
    test "$yaml_probe_count" -eq 3
    grep -q '"path_id":"path-direct"' "$OUT_DIR/yaml.jsonl"
    grep -q '"path_id":"path-via-hub"' "$OUT_DIR/yaml.jsonl"
    grep -q '"path_id":"path-via-relay-c"' "$OUT_DIR/yaml.jsonl"
    grep -q '"path_id":"path-via-relay-c".*"target":"203.0.113.26"' "$OUT_DIR/yaml.jsonl"
    ;;
esac
cat "$OUT_DIR/yaml.jsonl" | "$BUILD_DIR/eventnetd" "$YAML" \
  --intent "$INTENT_ID" --telemetry-stdin --batch-size "$yaml_probe_count" --count 1 > "$OUT_DIR/yaml-eventnetd.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/yaml-eventnetd.log"

if command -v ping >/dev/null 2>&1 && ping -c 1 -W 1 127.0.0.1 >/dev/null 2>&1; then
  "$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 127.0.0.1 \
    --count 1 --interval-ms 0 --output "$OUT_DIR/real-ping.jsonl"
  grep -q '"state":"healthy"' "$OUT_DIR/real-ping.jsonl"
fi

set +e
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 --count invalid >/dev/null 2>&1
invalid_count_status=$?
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 --append >/dev/null 2>&1
append_without_output_status=$?
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 --simulate nan 0 >/dev/null 2>&1
invalid_simulate_status=$?
"$BUILD_DIR/eventnet_agent" --source site-a --probe path-direct 203.0.113.9 --probe path-direct 203.0.113.13 >/dev/null 2>&1
duplicate_probe_status=$?
"$BUILD_DIR/eventnet_agent" --yaml "$YAML" --intent "$INTENT_ID" --source site-a >/dev/null 2>&1
yaml_source_status=$?
set -e
if [ "$invalid_count_status" -ne 2 ] || [ "$append_without_output_status" -ne 2 ] || [ "$invalid_simulate_status" -ne 2 ] || [ "$duplicate_probe_status" -ne 2 ] || [ "$yaml_source_status" -ne 2 ]; then
  printf 'Agent smoke failed: invalid Agent arguments were accepted.\n' >&2
  exit 1
fi

printf 'Agent smoke passed: healthy, failed, YAML-expanded, and available real-ping telemetry JSONL were generated.\n'
