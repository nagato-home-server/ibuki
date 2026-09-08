#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
YAML="${1:-$ROOT_DIR/samples/route-examples.yaml}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/relay-route-plan-smoke}"

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

if [ ! -x "$BUILD_DIR/eventnet_netns_plan" ]; then
  printf 'eventnet_netns_plan not found. Run sh scripts/vm-build-cc.sh first.\n' >&2
  exit 1
fi

OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --path path-relay-chain-routes > "$OUT_DIR/generate.log"
cat "$OUT_DIR/selected-path.txt"
cat "$OUT_DIR/vpp-route-plan.sh"

for route_id in chain-site-a-to-relay chain-relay-to-site-b chain-relay-return chain-site-b-return; do
  grep -q "explicit route $route_id" "$OUT_DIR/vpp-route-plan.sh"
done

route_count=$(grep -c "run_vpp ip route add" "$OUT_DIR/vpp-route-plan.sh")
if [ "$route_count" -ne 4 ]; then
  printf 'expected 4 relay route commands, got %s\n' "$route_count" >&2
  exit 1
fi

printf 'Relay route plan smoke passed: all four node-scoped routes were generated.\n'
