#!/usr/bin/env sh
set -u

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML=${1:-samples/linux-vm-netns.yaml}
REPEAT=${REPEAT:-5}
OUT_FILE=${OUT_FILE:-$ROOT_DIR/out/paper-metrics.csv}
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-paper-baseline}

if [ "$(id -u)" != "0" ]; then
  printf '%s\n' 'Please run as root: sudo sh scripts/vm-paper-collect-metrics.sh [yaml]' >&2
  exit 1
fi
case "$REPEAT" in
  ''|*[!0-9]*) printf 'invalid REPEAT: %s\n' "$REPEAT" >&2; exit 2 ;;
esac
[ "$REPEAT" -ge 1 ] || { printf '%s\n' 'REPEAT must be at least 1' >&2; exit 2; }

cd "$ROOT_DIR"
mkdir -p "$(dirname -- "$OUT_FILE")"
printf '%s\n' 'scenario,strategy,repetition,transition_ms,packet_loss,packet_reordering,tcp_retransmissions,cpu_percent,memory_mb' > "$OUT_FILE"
overall=0

now_ms() {
  value=$(date +%s%N 2>/dev/null || true)
  case "$value" in
    *N*|"") printf '%s000\n' "$(date +%s)" ;;
    *) printf '%s\n' "$((value / 1000000))" ;;
  esac
}

collect_case() {
  scenario="$1"
  mode="$2"
  repetition="$3"
  log_file="$OUT_FILE.$scenario.$repetition.log"
  start_ms=$(now_ms)
  set +e
  MODE="$mode" BUILD_DIR="$BUILD_DIR" sh scripts/vm-controller-integrated-runtime-smoke.sh "$YAML" >"$log_file" 2>&1
  status=$?
  set -e
  end_ms=$(now_ms)
  transition_ms=$((end_ms - start_ms))
  packet_loss=$(awk '/packet loss/ { gsub(/%/, "", $6); print $6; exit }' "$log_file")
  [ -n "$packet_loss" ] || packet_loss=100
  if [ "$status" -ne 0 ]; then
    printf 'metric collection failed: %s repetition %s (status %s), see %s\n' "$scenario" "$repetition" "$status" "$log_file" >&2
    overall=1
  fi
  printf '%s,%s,%s,%s,%s,,,,\n' "$scenario" "$mode" "$repetition" "$transition_ms" "$packet_loss" >> "$OUT_FILE"
  sh scripts/vm-netns-ipsec-direct-stop.sh >/dev/null 2>&1 || true
  sh scripts/vm-netns-ipsec-hub-stop.sh >/dev/null 2>&1 || true
}

set -e
for repetition in $(seq 1 "$REPEAT"); do
  printf 'collect: direct %s/%s\n' "$repetition" "$REPEAT"
  collect_case direct direct "$repetition"
  printf 'collect: fallback %s/%s\n' "$repetition" "$REPEAT"
  collect_case hub-fallback fallback "$repetition"
done

printf 'metrics written: %s\n' "$OUT_FILE"
exit "$overall"
