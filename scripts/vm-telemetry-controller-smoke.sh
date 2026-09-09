#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/telemetry-controller-smoke}"

cd "$ROOT_DIR"
if [ "${DEBUG_VALIDATION:-0}" = "1" ]; then set -x; fi
mkdir -p "$OUT_DIR"
if [ ! -x "$BUILD_DIR/eventnet_agent" ] || [ ! -x "$BUILD_DIR/eventnetd" ]; then sh scripts/vm-build-cc.sh; fi

"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 --count 1 --simulate 12 0 --output "$OUT_DIR/direct.jsonl"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$OUT_DIR/direct.jsonl" \
  --status-jsonl "$OUT_DIR/direct-status.jsonl" > "$OUT_DIR/direct.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/direct.log"
grep -q '"schema":"ibuki.status.v1"' "$OUT_DIR/direct-status.jsonl"
if [ "$(uname -s)" = "Linux" ]; then
  telemetry_target="$OUT_DIR/telemetry-target.jsonl"
  telemetry_link="$OUT_DIR/telemetry-link.jsonl"
  cp "$OUT_DIR/direct.jsonl" "$telemetry_target"
  ln -s "$(basename "$telemetry_target")" "$telemetry_link"
  if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$telemetry_link" --once > "$OUT_DIR/telemetry-link.log" 2>&1; then
    printf '%s\n' 'telemetry symlink unexpectedly accepted' >&2
    exit 1
  fi
  test -s "$OUT_DIR/telemetry-link.log"
  rm -f "$telemetry_link" "$telemetry_target"
  cp "$OUT_DIR/direct.jsonl" "$telemetry_target"
  chmod 0666 "$telemetry_target"
  if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$telemetry_target" --once > "$OUT_DIR/telemetry-permission.log" 2>&1; then
    printf '%s\n' 'writable telemetry file unexpectedly accepted' >&2
    exit 1
  fi
  test -s "$OUT_DIR/telemetry-permission.log"
  chmod 0600 "$telemetry_target"
  rm -f "$telemetry_target"
  dd if=/dev/zero bs=1M count=5 2>/dev/null | tr '\000' '\n' > "$OUT_DIR/telemetry-too-large.jsonl"
  if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$OUT_DIR/telemetry-too-large.jsonl" --once > "$OUT_DIR/telemetry-too-large.log" 2>&1; then
    printf '%s\n' 'oversized telemetry unexpectedly accepted' >&2
    exit 1
  fi
  test -s "$OUT_DIR/telemetry-too-large.log"
  rm -f "$OUT_DIR/telemetry-too-large.jsonl"
fi
"$BUILD_DIR/eventnet_agent" --path path-direct --target 203.0.113.9 --count 1 --simulate 12 0 --output "$OUT_DIR/no-source.jsonl"
if grep -q '"source"' "$OUT_DIR/no-source.jsonl"; then
  printf '%s\n' 'source was emitted despite being omitted' >&2
  exit 1
fi
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$OUT_DIR/no-source.jsonl" > "$OUT_DIR/no-source.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/no-source.log"

"$BUILD_DIR/eventnet_agent" --source site-a \
  --probe path-direct 203.0.113.9 --probe path-via-hub 203.0.113.13 \
  --count 1 --interval-ms 0 --simulate 12 0 --output "$OUT_DIR/multi-probe.jsonl"
[ "$(grep -c '"schema":"ibuki.telemetry.path_health.v1"' "$OUT_DIR/multi-probe.jsonl")" -eq 2 ]
cat "$OUT_DIR/multi-probe.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry-stdin --batch-size 2 --count 1 > "$OUT_DIR/multi-probe.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/multi-probe.log"

"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 1 --simulate 12 0 --output "$OUT_DIR/round1-direct.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-via-hub --source site-a --target 203.0.113.13 \
  --count 1 --simulate 30 0 --output "$OUT_DIR/round1-hub.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 1 --simulate 0 100 --output "$OUT_DIR/round2-direct.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-via-hub --source site-a --target 203.0.113.13 \
  --count 1 --simulate 30 0 --output "$OUT_DIR/round2-hub.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 1 --simulate 12 0 --output "$OUT_DIR/round3-direct.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-via-hub --source site-a --target 203.0.113.13 \
  --count 1 --simulate 30 0 --output "$OUT_DIR/round3-hub.jsonl"
cat "$OUT_DIR/round1-direct.jsonl" "$OUT_DIR/round1-hub.jsonl" \
  "$OUT_DIR/round2-direct.jsonl" "$OUT_DIR/round2-hub.jsonl" \
  "$OUT_DIR/round3-direct.jsonl" "$OUT_DIR/round3-hub.jsonl" | \
  "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-stdin --batch-size 2 --count 3 --max-age-ms 0 > "$OUT_DIR/multi-round.log"
[ "$(grep -c 'selected_path: path-direct' "$OUT_DIR/multi-round.log")" -eq 2 ]
[ "$(grep -c 'selected_path: path-via-hub' "$OUT_DIR/multi-round.log")" -eq 1 ]
python3 - "$OUT_DIR/direct-status.jsonl" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    records = [json.loads(line) for line in source if line.strip()]
assert records, "status JSONL is empty"
record = records[-1]
assert record["schema"] == "ibuki.status.v1"
for key in ("timestamp_ms", "intent_id", "selected_path", "transition_state", "reason"):
    assert key in record, key
assert isinstance(record["excluded"], list)
for excluded in record["excluded"]:
    assert isinstance(excluded, dict)
    assert excluded["path_id"]
    assert excluded["reason"]
PY

printf '%s\n' 'corrupt-state' > "$OUT_DIR/corrupt-state.tsv"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/direct.jsonl" --state-file "$OUT_DIR/corrupt-state.tsv" > "$OUT_DIR/corrupt-state.log" 2>&1; then
  printf '%s\n' 'corrupt state unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'state restore failed' "$OUT_DIR/corrupt-state.log"

printf '%s\n' 'site-a->site-b	path-direct' 'site-a->site-b	path-via-hub' > "$OUT_DIR/duplicate-state.tsv"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/direct.jsonl" --state-file "$OUT_DIR/duplicate-state.tsv" > "$OUT_DIR/duplicate-state.log" 2>&1; then
  printf '%s\n' 'duplicate state unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'state restore failed' "$OUT_DIR/duplicate-state.log"

"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 --count 1 --simulate 0 100 --output "$OUT_DIR/failed.jsonl"
"$BUILD_DIR/eventnet_agent" --path path-via-hub --source site-a --target 203.0.113.13 --count 1 --simulate 30 0 --output "$OUT_DIR/hub.jsonl"
cat "$OUT_DIR/hub.jsonl" >> "$OUT_DIR/failed.jsonl"
cat "$OUT_DIR/failed.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry-stdin --batch-size 2 --count 1 > "$OUT_DIR/failed.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/failed.log"

cat "$OUT_DIR/failed.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry-stdin --batch-size 2 --count 1 > "$OUT_DIR/batch.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/batch.log"

stale_timestamp_ms=$((($(date +%s) - 600) * 1000))
fresh_timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-direct","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":%s}\n' "$stale_timestamp_ms" > "$OUT_DIR/stale-direct.jsonl"
printf '{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-via-hub","state":"healthy","rtt_ms":20,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":%s}\n' "$fresh_timestamp_ms" >> "$OUT_DIR/stale-direct.jsonl"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/stale-direct.jsonl" --max-age-ms 30000 > "$OUT_DIR/stale.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/stale.log"

cat "$OUT_DIR/direct.jsonl" "$OUT_DIR/direct.jsonl" > "$OUT_DIR/repeated.jsonl"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$OUT_DIR/repeated.jsonl" --count 2 --interval-ms 1 > "$OUT_DIR/repeated.log"
grep -q 'eventnetd_iteration: 1/count' "$OUT_DIR/repeated.log"
grep -q 'eventnetd_iteration: 2/count' "$OUT_DIR/repeated.log"

cat "$OUT_DIR/direct.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry-stdin --count 1 > "$OUT_DIR/stdin.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/stdin.log"

SOCKET_PATH="$OUT_DIR/eventnetd.sock"
rm -f "$SOCKET_PATH"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-socket "$SOCKET_PATH" --backend command --count 1 > "$OUT_DIR/socket.log" 2>&1 &
DAEMON_PID=$!
trap 'kill "$DAEMON_PID" 2>/dev/null || true; rm -f "$SOCKET_PATH"' EXIT
for attempt in $(seq 1 50); do
  [ -S "$SOCKET_PATH" ] && break
  sleep 0.1
done
[ -S "$SOCKET_PATH" ]
python3 - "$SOCKET_PATH" "$OUT_DIR/direct.jsonl" <<'PY'
import socket
import sys

path, telemetry = sys.argv[1:]
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.connect(path)
    with open(telemetry, "rb") as source:
        client.sendall(source.read())
PY
wait "$DAEMON_PID"
grep -q 'selected_path: path-direct' "$OUT_DIR/socket.log"
grep -q '\[dry-run\] swanctl --initiate --child tun-a-b' "$OUT_DIR/socket.log"
rm -f "$SOCKET_PATH"
trap - EXIT

set +e
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --telemetry "$OUT_DIR/direct.jsonl" --count invalid >/dev/null 2>&1
invalid_count_status=$?
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --telemetry "$OUT_DIR/direct.jsonl" --batch-size 0 >/dev/null 2>&1
invalid_batch_status=$?
set -e
if [ "$invalid_count_status" -ne 2 ] || [ "$invalid_batch_status" -ne 2 ]; then
  printf 'Telemetry controller smoke failed: invalid eventnetd CLI values were accepted.\n' >&2
  exit 1
fi

printf 'Telemetry controller smoke passed: Agent measurements, batching, freshness, and control-plane inputs were handled.\n'
