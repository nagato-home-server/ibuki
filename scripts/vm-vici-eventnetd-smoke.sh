#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
VICI_BUILD_DIR="${VICI_BUILD_DIR:-$ROOT_DIR/build-vici}"
YAML="${YAML:-samples/linux-vm-netns.yaml}"
INTENT_ID="${INTENT_ID:-intent-a-b}"
VICI_URI="${VICI_URI:-unix:///run/strongswan/charon.vici}"
VICI_CHILD="${VICI_CHILD:-tun-a-b}"
VICI_PATH="${VICI_PATH:-path-direct}"
DURATION_MS="${VICI_DURATION_MS:-10000}"
RETRY_COUNT="${VICI_RETRY_COUNT:-2}"
BACKOFF_MS="${VICI_BACKOFF_MS:-250}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/vici-eventnetd-smoke}"
RUN_CONTROLLER_PROBE="${RUN_CONTROLLER_PROBE:-0}"
RUN_INTERNAL="${RUN_INTERNAL:-0}"
CONTROLLER_PROBE_YAML="${CONTROLLER_PROBE_YAML:-samples/cert-auth.yaml}"
CONTROLLER_PROBE_INTENT="${CONTROLLER_PROBE_INTENT:-intent-cert-a-b}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo VICI_URI=%s sh %s\n' "$VICI_URI" "$0" >&2
  exit 1
fi
for binary in "$BUILD_DIR/eventnetd" "$VICI_BUILD_DIR/eventnet_strongswan_vici_probe"; do
  if [ ! -x "$binary" ]; then
    printf 'missing executable: %s\n' "$binary" >&2
    exit 2
  fi
done

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/eventnetd.log" "$OUT_DIR/monitor.jsonl" "$OUT_DIR/initiate.log" "$OUT_DIR/terminate.log"

if [ "$RUN_CONTROLLER_PROBE" = "1" ]; then
  controller_probe="$VICI_BUILD_DIR/eventnet_strongswan_vici_controller_probe"
  if [ ! -x "$controller_probe" ]; then
    printf 'missing executable: %s\n' "$controller_probe" >&2
    exit 2
  fi
  if ! "$controller_probe" "$VICI_URI" "$CONTROLLER_PROBE_YAML" "$CONTROLLER_PROBE_INTENT" > "$OUT_DIR/controller-probe.log" 2>&1 ||
    ! grep -q '^VICI controller reconcile passed$' "$OUT_DIR/controller-probe.log"; then
    cat "$OUT_DIR/controller-probe.log" >&2
    exit 1
  fi
  printf 'VICI controller probe passed: %s\n' "$OUT_DIR/controller-probe.log"
fi

if [ "$RUN_INTERNAL" = "1" ] && [ ! -x "$VICI_BUILD_DIR/eventnetd" ]; then
  printf 'missing VICI-enabled eventnetd: %s\n' "$VICI_BUILD_DIR/eventnetd" >&2
  exit 2
fi

printf 'Starting VICI monitor and eventnetd...\n'
set +e
if [ "$RUN_INTERNAL" = "1" ]; then
  "$VICI_BUILD_DIR/eventnetd" "$YAML" --intent "$INTENT_ID" --backend command --swanctl-uri "$VICI_URI" \
    --vici-monitor-child "$VICI_CHILD" --vici-monitor-path "$VICI_PATH" \
    --vici-monitor-duration-ms "$DURATION_MS" --state-file "$OUT_DIR/state.tsv" \
    > "$OUT_DIR/eventnetd.log" 2>&1 &
  PIPELINE_PID=$!
else
"$VICI_BUILD_DIR/eventnet_strongswan_vici_probe" "$VICI_URI" monitor "$VICI_CHILD" "$VICI_PATH" \
  "$DURATION_MS" "$RETRY_COUNT" "$BACKOFF_MS" > "$OUT_DIR/monitor.jsonl" 2> "$OUT_DIR/monitor.log" | \
  "$BUILD_DIR/eventnetd" "$YAML" --intent "$INTENT_ID" --telemetry-stdin --count 0 --max-age-ms 0 \
  > "$OUT_DIR/eventnetd.log" 2>&1 &
PIPELINE_PID=$!
fi
sleep 1

if ! "$VICI_BUILD_DIR/eventnet_strongswan_vici_probe" "$VICI_URI" terminate "$VICI_CHILD" > "$OUT_DIR/terminate.log" 2>&1; then
  printf 'VICI terminate failed; continuing with initiate.\n' >&2
fi
if ! "$VICI_BUILD_DIR/eventnet_strongswan_vici_probe" "$VICI_URI" initiate "$VICI_CHILD" > "$OUT_DIR/initiate.log" 2>&1; then
  printf 'VICI initiate failed.\n' >&2
  kill "$PIPELINE_PID" 2>/dev/null || true
  wait "$PIPELINE_PID" 2>/dev/null || true
  cat "$OUT_DIR/initiate.log" >&2
  exit 1
fi
wait "$PIPELINE_PID"
PIPELINE_STATUS=$?
set -e

if [ "$PIPELINE_STATUS" -ne 0 ]; then
  cat "$OUT_DIR/monitor.log" >&2 || true
  cat "$OUT_DIR/eventnetd.log" >&2 || true
  exit "$PIPELINE_STATUS"
fi
grep -q "selected_path: $VICI_PATH" "$OUT_DIR/eventnetd.log"
if [ "$RUN_INTERNAL" = "1" ]; then
  grep -q 'vici_monitor_completed:' "$OUT_DIR/eventnetd.log"
else
  grep -q 'telemetry_records:' "$OUT_DIR/eventnetd.log"
fi

if [ "$RUN_INTERNAL" = "1" ]; then
  printf 'VICI eventnetd smoke passed: eventnetd internal monitor selected %s.\n' "$VICI_PATH"
else
  printf 'VICI eventnetd smoke passed: monitor events reached eventnetd and selected %s.\n' "$VICI_PATH"
fi
printf '  monitor: %s\n' "$OUT_DIR/monitor.jsonl"
printf '  eventnetd: %s\n' "$OUT_DIR/eventnetd.log"
