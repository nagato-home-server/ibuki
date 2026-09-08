#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/stability-smoke}
TELEMETRY=$OUT_DIR/telemetry.jsonl
LOG=$OUT_DIR/eventnetd.log
STATE=$OUT_DIR/state.tsv

cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnetd" ]; then sh scripts/vm-build-cc.sh; fi
mkdir -p "$OUT_DIR"
timestamp_ms=$(($(date +%s) * 1000))
cat > "$TELEMETRY" <<EOF
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-legacy-single-route","state":"healthy","rtt_ms":5,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-priority-backup","state":"healthy","rtt_ms":10,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-legacy-single-route","state":"healthy","rtt_ms":10,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-priority-backup","state":"healthy","rtt_ms":9,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-legacy-single-route","state":"healthy","rtt_ms":10,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-priority-backup","state":"healthy","rtt_ms":5,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$timestamp_ms,"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-legacy-single-route","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$((timestamp_ms - 1000)),"consecutive_successes":3,"consecutive_failures":0}
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-priority-backup","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":1,"timestamp_ms":$((timestamp_ms - 1000)),"consecutive_successes":3,"consecutive_failures":0}
EOF

cat "$TELEMETRY" | "$BUILD_DIR/eventnetd" samples/route-examples.yaml --intent intent-evaluated-stability \
  --telemetry-stdin --batch-size 2 --count 4 --state-file "$STATE" > "$LOG"
test "$(grep -c 'selected_path: path-legacy-single-route' "$LOG")" -eq 2
test "$(grep -c 'selected_path: path-priority-backup' "$LOG")" -eq 2
grep -q 'active path retained by hysteresis' "$LOG"
printf '%s\n' 'Stability smoke passed: hysteresis and out-of-order telemetry protection behaved as expected.'
