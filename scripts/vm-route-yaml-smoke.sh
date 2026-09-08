#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
YAML="${1:-samples/route-examples.yaml}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/route-yaml-smoke}"

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"

if [ ! -x "$BUILD_DIR/eventnet_yaml_demo" ] || [ ! -x "$BUILD_DIR/eventnet_netns_plan" ]; then
  sh scripts/vm-build-cc.sh
fi

run_intent() {
  intent="$1"
  expected="$2"
  log="$OUT_DIR/$intent.log"
  printf '\n== route yaml: %s ==\n' "$intent"
  "$BUILD_DIR/eventnet_yaml_demo" --intent "$intent" --conf "$OUT_DIR/$intent-swanctl.conf" "$YAML" > "$log"
  cat "$log"
  grep -q "selected_path: $expected" "$log"
}

run_path_plan() {
  path="$1"
  expected="$2"
  path_out="$OUT_DIR/$path"
  log="$OUT_DIR/$path-netns-plan.log"
  printf '\n== route plan: %s ==\n' "$path"
  OUT_DIR="$path_out" sh scripts/vm-generate-netns-runtime.sh "$YAML" --path "$path" > "$log"
  cat "$path_out/selected-path.txt"
  cat "$path_out/vpp-route-plan.sh"
  grep -q "$expected" "$path_out/vpp-route-plan.sh"
}

run_intent intent-legacy-single-route path-legacy-single-route
run_intent intent-explicit-single-route path-explicit-single-route
run_intent intent-bidirectional-routes path-bidirectional-routes
run_intent intent-hub-node-routes path-hub-node-routes
run_intent intent-route-attributes path-route-attributes
grep -q 'vppctl ip route add 10.10.2.0/24 table 100 preference 20 via 203.0.113.9 ipsec0' "$OUT_DIR/intent-route-attributes.log"
run_intent intent-relay-chain-routes path-relay-chain-routes
run_intent intent-asymmetric-routes path-asymmetric-routes
run_intent intent-priority-backup path-legacy-single-route
run_intent intent-vlan-security-hub path-hub-node-routes
run_intent intent-vlan-direct path-legacy-single-route

run_path_plan path-legacy-single-route "ip route add 10.10.2.0/24 via 203.0.113.9"
run_path_plan path-explicit-single-route "explicit route dst-to-site-b"
run_path_plan path-bidirectional-routes "explicit route return-to-site-a"
run_path_plan path-hub-node-routes "explicit route hub-to-site-b"
run_path_plan path-route-attributes "table 100 preference 20"
grep -q 'VPPCTL_SOCKET="${VPPCTL_SOCKET:-}"' "$OUT_DIR/path-route-attributes/vpp-route-plan.sh"
grep -q '"$VPPCTL" -s "$VPPCTL_SOCKET"' "$OUT_DIR/path-route-attributes/vpp-route-plan.sh"
grep -q 'run_vpp show ip fib' "$OUT_DIR/path-route-attributes/vpp-route-plan.sh"
run_path_plan path-relay-chain-routes "explicit route chain-relay-to-site-b"
run_path_plan path-asymmetric-routes "explicit route asymmetric-return"

legacy_out="$OUT_DIR/legacy-no-segment"
printf '\n== legacy route without segments ==\n'
OUT_DIR="$legacy_out" sh scripts/vm-generate-netns-runtime.sh "$ROOT_DIR/samples/agent-legacy-no-segment.yaml" \
  --intent intent-legacy-no-segment > "$OUT_DIR/legacy-no-segment.log"
cat "$legacy_out/vpp-route-plan.sh"
grep -q 'legacy destination route' "$legacy_out/vpp-route-plan.sh"
grep -q 'ip route add 10.10.2.0/24 via 192.0.2.2' "$legacy_out/vpp-route-plan.sh"

printf '\n== invalid route yaml is rejected ==\n'
if "$BUILD_DIR/eventnet_yaml_demo" "$ROOT_DIR/samples/route-invalid-examples.yaml" > "$OUT_DIR/invalid.log" 2>&1; then
  cat "$OUT_DIR/invalid.log"
  printf 'invalid route yaml unexpectedly succeeded\n' >&2
  exit 1
fi
grep -q "path route requires next_hop" "$OUT_DIR/invalid.log"
cat "$OUT_DIR/invalid.log"

printf '\n== segmentless boundary cases ==\n'
if "$BUILD_DIR/eventnet_yaml_demo" --validate-only "$ROOT_DIR/samples/invalid-empty-no-segment.yaml" > "$OUT_DIR/empty-no-segment.log" 2>&1; then
  cat "$OUT_DIR/empty-no-segment.log"
  printf '%s\n' 'empty segmentless path unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'path without segments requires a legacy route or explicit routes' "$OUT_DIR/empty-no-segment.log"
cat "$OUT_DIR/empty-no-segment.log"

explicit_out="$OUT_DIR/explicit-no-segment"
if OUT_DIR="$explicit_out" sh scripts/vm-generate-netns-runtime.sh "$ROOT_DIR/samples/legacy-explicit-no-segment.yaml" \
  --intent intent-explicit-no-segment > "$OUT_DIR/explicit-no-segment.log" 2>&1; then
  cat "$OUT_DIR/explicit-no-segment.log"
  printf '%s\n' 'ambiguous explicit segmentless path unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'unsupported' "$OUT_DIR/explicit-no-segment.log"
cat "$OUT_DIR/explicit-no-segment.log"

printf '\nRoute YAML smoke passed: legacy, explicit, bidirectional, node-scoped, attributed, relay-chain, asymmetric, backup, and invalid-route cases are covered.\n'
