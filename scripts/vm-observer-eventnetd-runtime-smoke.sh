#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
YAML="${1:-samples/linux-vm-netns.yaml}"
INTENT_ID="${INTENT_ID:-intent-a-b}"
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
VPP_SOCKET="${VPP_SOCKET:-}"
VPP_TABLE_ID="${VPP_TABLE_ID:-}"
PATH_ID="${PATH_ID:-path-direct}"
CHILD_ID="${CHILD_ID:-tun-a-b}"
ROUTE_PREFIX="${ROUTE_PREFIX:-10.10.2.0/24}"
ROUTE_ID="${ROUTE_ID:-route-a-b}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/observer-eventnetd-runtime}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s [yaml]\n' "$0" >&2
  exit 1
fi

cd "$ROOT_DIR"
for binary in eventnetd eventnet_swanctl_observer eventnet_vpp_observer; do
  if [ ! -x "$BUILD_DIR/$binary" ]; then
    printf 'missing binary: %s. Run sh scripts/vm-build-cc.sh first.\n' "$BUILD_DIR/$binary" >&2
    exit 1
  fi
done

SWAN_URI="unix://$RUN_BASE/site-a/charon.vici"
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/events.jsonl" "$OUT_DIR/eventnetd.log"

if [ ! -S "$RUN_BASE/site-a/charon.vici" ]; then
  printf 'missing strongSwan VICI socket: %s\n' "$SWAN_URI" >&2
  exit 1
fi

swanctl --list-sas --uri "$SWAN_URI" > "$OUT_DIR/list-sas.txt"
"$BUILD_DIR/eventnet_swanctl_observer" "$OUT_DIR/list-sas.txt" "$CHILD_ID" --event "$PATH_ID" >> "$OUT_DIR/events.jsonl"

if [ -n "$VPP_SOCKET" ]; then
  vppctl -s "$VPP_SOCKET" show ip fib > "$OUT_DIR/show-ip-fib.txt"
else
  vppctl show ip fib > "$OUT_DIR/show-ip-fib.txt"
fi
if [ -n "$VPP_TABLE_ID" ]; then
  case "$VPP_TABLE_ID" in
    *[!0-9]*) printf 'invalid VPP_TABLE_ID: %s\n' "$VPP_TABLE_ID" >&2; exit 1 ;;
  esac
  "$BUILD_DIR/eventnet_vpp_observer" "$OUT_DIR/show-ip-fib.txt" "$ROUTE_PREFIX" --event "$PATH_ID" "$ROUTE_ID" --table "$VPP_TABLE_ID" >> "$OUT_DIR/events.jsonl"
else
  "$BUILD_DIR/eventnet_vpp_observer" "$OUT_DIR/show-ip-fib.txt" "$ROUTE_PREFIX" --event "$PATH_ID" "$ROUTE_ID" >> "$OUT_DIR/events.jsonl"
fi

"$BUILD_DIR/eventnetd" "$YAML" --intent "$INTENT_ID" --telemetry "$OUT_DIR/events.jsonl" --once > "$OUT_DIR/eventnetd.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/eventnetd.log"
grep -q 'telemetry_records: 2' "$OUT_DIR/eventnetd.log"

printf 'Observer eventnetd runtime smoke passed: strongSwan and VPP events were accepted.\n'
printf '  events: %s\n' "$OUT_DIR/events.jsonl"
printf '  log: %s\n' "$OUT_DIR/eventnetd.log"
