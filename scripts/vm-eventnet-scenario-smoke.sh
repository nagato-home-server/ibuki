#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
YAML="${1:-samples/linux-vm-netns.yaml}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/scenario}"

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

if [ ! -x "$BUILD_DIR/eventnet_scenario" ]; then
  printf 'eventnet_scenario not found. Run sh scripts/vm-build-cc.sh first.\n' >&2
  exit 1
fi

printf '== scenario: priority selects direct ==\n'
"$BUILD_DIR/eventnet_scenario" "$YAML" \
  --expect path-direct

printf '\n== scenario: direct failure falls back to hub ==\n'
"$BUILD_DIR/eventnet_scenario" "$YAML" \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub

printf '\n== scenario: evaluated prefers lower loss/latency relay ==\n'
"$BUILD_DIR/eventnet_scenario" "$YAML" \
  --mode evaluated \
  --health path-direct=healthy,rtt=80,loss=0.5 \
  --health path-via-relay-c=healthy,rtt=30,loss=0.1 \
  --health path-via-hub=healthy,rtt=50,loss=0.2 \
  --compare packet_loss,latency,hop_count,path_id \
  --expect path-via-relay-c

printf '\n== scenario: direct failure can generate runtime ==\n'
"$BUILD_DIR/eventnet_scenario" "$YAML" \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub \
  --generate-runtime

printf '\n== scenario: multi-step direct -> fallback -> recovery -> relay-best ==\n'
"$BUILD_DIR/eventnet_scenario" "$YAML" \
  --step direct-ok \
  --step direct-failed \
  --step direct-recovered \
  --step relay-best \
  --explain-json "$OUT_DIR/multistep-explain.jsonl"

grep -q '"selected_path":"path-via-hub"' "$OUT_DIR/multistep-explain.jsonl"
grep -q '"selected_path":"path-via-relay-c"' "$OUT_DIR/multistep-explain.jsonl"
printf 'explain_jsonl_written: %s\n' "$OUT_DIR/multistep-explain.jsonl"

set +e
"$BUILD_DIR/eventnet_scenario" "$YAML" --health 'path-direct=healthy,rtt=nan,loss=0' >/dev/null 2>&1
invalid_health_status=$?
set -e
if [ "$invalid_health_status" -ne 2 ]; then
  printf 'Scenario smoke failed: invalid health values were accepted.\n' >&2
  exit 1
fi

printf '\nEventNet scenario smoke passed.\n'
