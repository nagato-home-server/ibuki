#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML="${1:-samples/linux-vm-netns.yaml}"

cd "$ROOT_DIR"

printf '== VPP route plan: controller-selected default path ==\n'
sh scripts/vm-generate-netns-runtime.sh "$YAML"
cat out/netns-runtime/selected-path.txt
DRY_RUN=1 sh out/netns-runtime/vpp-route-plan.sh

printf '\n== VPP VLAN route plan: hub waypoint ==\n'
VLAN_OUT_DIR="out/netns-runtime-vlan-hub"
OUT_DIR="$VLAN_OUT_DIR" sh scripts/vm-generate-netns-runtime.sh samples/vpp-vlan-hub-netns.yaml --intent intent-vlan-hub
grep -q '^selected_path: path-vlan-hub$' "$VLAN_OUT_DIR/selected-path.txt"
grep -q 'ensure_vlan_subinterface host-vpp-site-a 100' "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ensure_vlan_subinterface host-vpp-hub-1 100' "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ensure_vlan_subinterface host-vpp-site-b 100' "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ensure_unmatched_vlan_acl host-vpp-hub-1' "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ip route add 10.10.2.0/24 table 100 via 172.16.103.2 host-vpp-hub-b.100' "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"
DRY_RUN=1 sh "$VLAN_OUT_DIR/vpp-netns-route-plan.sh"

printf '\n== VPP route plan: direct failure fallback hub path ==\n'
sh scripts/vm-generate-netns-runtime.sh "$YAML" --active-path path-direct --fail-path path-direct
cat out/netns-runtime/selected-path.txt
DRY_RUN=1 sh out/netns-runtime/vpp-route-plan.sh

printf '\nVPP route plan smoke passed: direct, hub fallback, and VLAN hub plans generated correctly.\n'
