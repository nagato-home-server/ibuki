#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/agent-eventnetd-live-smoke}"
YAML="${1:-$ROOT_DIR/samples/linux-vm-netns.yaml}"
INTENT_ID="${INTENT_ID:-intent-a-b}"
COUNT="${LIVE_COUNT:-4}"
INTERVAL_MS="${LIVE_INTERVAL_MS:-250}"

case "$COUNT" in ''|*[!0-9]*|0) printf '%s\n' 'LIVE_COUNT must be a positive integer.' >&2; exit 2 ;; esac
case "$INTERVAL_MS" in ''|*[!0-9]*) printf '%s\n' 'LIVE_INTERVAL_MS must be a non-negative integer.' >&2; exit 2 ;; esac
cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnet_agent" ] || [ ! -x "$BUILD_DIR/eventnetd" ]; then
  sh scripts/vm-build-cc.sh
fi
mkdir -p "$OUT_DIR"
telemetry="$OUT_DIR/live.jsonl"
status_jsonl="$OUT_DIR/status.jsonl"
state_file="$OUT_DIR/state.tsv"
eventnetd_log="$OUT_DIR/eventnetd.log"
rm -f "$telemetry" "$status_jsonl" "$state_file" "$eventnetd_log"

write_record() {
  "$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
    --count 1 --interval-ms 0 --simulate 12 0 --append --output "$telemetry"
}

write_record
writer_pid=''
cleanup() {
  if [ -n "$writer_pid" ]; then
    kill "$writer_pid" 2>/dev/null || true
    wait "$writer_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

(
  iteration=1
  while [ "$iteration" -lt "$COUNT" ]; do
    sleep_seconds=$(awk -v milliseconds="$INTERVAL_MS" 'BEGIN { printf "%.3f", milliseconds / 1000 }')
    sleep "$sleep_seconds"
    write_record
    iteration=$((iteration + 1))
  done
) &
writer_pid=$!

"$BUILD_DIR/eventnetd" "$YAML" --intent "$INTENT_ID" --telemetry "$telemetry" \
  --batch-size 1 --count "$COUNT" --interval-ms "$INTERVAL_MS" \
  --state-file "$state_file" --status-jsonl "$status_jsonl" > "$eventnetd_log" 2>&1
wait "$writer_pid"
writer_pid=''

[ "$(wc -l < "$telemetry" | tr -d ' ')" -eq "$COUNT" ]
[ "$(wc -l < "$status_jsonl" | tr -d ' ')" -eq "$COUNT" ]
grep -q "eventnetd_iteration: $COUNT/" "$eventnetd_log"
printf '%s\n' "Agent/eventnetd live smoke passed: $COUNT records, interval ${INTERVAL_MS}ms."
