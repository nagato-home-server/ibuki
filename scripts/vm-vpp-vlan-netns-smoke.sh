#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VLAN_ID="${VLAN_ID:-100}"
VPP_TABLE_ID="${VPP_TABLE_ID:-0}"
YAML="${YAML:-samples/vpp-vlan-netns.yaml}"

if [ -n "${VLAN_MATRIX:-}" ] && [ "${VLAN_MATRIX_RUN:-0}" != "1" ]; then
  if [ "$(id -u)" != "0" ]; then
    printf 'Please run as root: sudo VLAN_MATRIX="%s" sh %s\n' "$VLAN_MATRIX" "$0" >&2
    exit 1
  fi
  matrix_root="${OUT_DIR:-$ROOT_DIR/out/vpp-vlan-netns}"
  mkdir -p "$matrix_root"
  matrix_summary="$matrix_root/matrix-summary.txt"
  : > "$matrix_summary"
  for matrix_vlan in $VLAN_MATRIX; do
    matrix_out="$matrix_root/vlan-$matrix_vlan"
    if VLAN_ID="$matrix_vlan" VLAN_MATRIX= VLAN_MATRIX_RUN=1 OUT_DIR="$matrix_out" sh "$0"; then
      printf 'vlan=%s status=pass output=%s\n' "$matrix_vlan" "$matrix_out" >> "$matrix_summary"
    else
      printf 'vlan=%s status=fail output=%s\n' "$matrix_vlan" "$matrix_out" >> "$matrix_summary"
      printf 'VLAN matrix failed at VLAN %s; summary: %s\n' "$matrix_vlan" "$matrix_summary" >&2
      exit 1
    fi
  done
  printf 'VLAN matrix passed; summary: %s\n' "$matrix_summary"
  exit 0
fi

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo VLAN_ID=%s sh %s\n' "$VLAN_ID" "$0" >&2
  exit 1
fi
case "$VLAN_ID" in
  ''|*[!0-9]*) printf 'invalid VLAN_ID: %s\n' "$VLAN_ID" >&2; exit 2 ;;
esac
if [ "$VLAN_ID" -lt 1 ] || [ "$VLAN_ID" -gt 4094 ]; then
  printf 'VLAN_ID must be between 1 and 4094\n' >&2
  exit 2
fi
case "$VPP_TABLE_ID" in
  ''|*[!0-9]*) printf 'invalid VPP_TABLE_ID: %s\n' "$VPP_TABLE_ID" >&2; exit 2 ;;
esac
if [ "$VPP_TABLE_ID" -gt 2147483647 ]; then
  printf 'VPP_TABLE_ID is too large: %s\n' "$VPP_TABLE_ID" >&2
  exit 2
fi
cd "$ROOT_DIR"
check_fib_route() {
  prefix="$1"
  next_hop="$2"
  table_id="$3"
  awk -v prefix="$prefix" -v next_hop="$next_hop" '
    /ipv4-VRF:/ {
      table = $0
      sub(/^.*ipv4-VRF:/, "", table)
      sub(/,.*/, "", table)
      next
    }
    $0 == prefix && (table == wanted_table || wanted_table == "") { in_route = 1; next }
    in_route && $0 ~ ("via " next_hop) { matched = 1; next }
    in_route && $0 !~ /^[[:space:]]/ { in_route = 0 }
    END { exit matched ? 0 : 1 }
  ' wanted_table="$table_id" "$OUT_DIR/show-ip-fib.txt"
}
sh scripts/vm-vpp-netns-setup.sh

OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/vpp-vlan-netns}"
mkdir -p "$OUT_DIR"
RUNTIME_YAML="$OUT_DIR/vpp-vlan-netns.yaml"
sed -e "s/^      vlan_id: 100$/      vlan_id: $VLAN_ID/" \
    -e "s/^        table: 0$/        table: $VPP_TABLE_ID/" \
    "$YAML" > "$RUNTIME_YAML"
OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$RUNTIME_YAML" --intent intent-vpp-vlan
grep -q '^selected_path: path-vpp-vlan$' "$OUT_DIR/selected-path.txt"
grep -q "vlan_id: $VLAN_ID" "$OUT_DIR/selected-path.txt"
if [ "$VPP_TABLE_ID" -gt 0 ]; then
  grep -q "ensure_vpp_table $VPP_TABLE_ID" "$OUT_DIR/vpp-route-plan.sh"
  grep -q "ensure_vpp_table $VPP_TABLE_ID" "$OUT_DIR/vpp-netns-route-plan.sh"
fi
grep -q "ip route add .*\\.$VLAN_ID" "$OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'set acl-plugin acl index' "$OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'set acl-plugin interface .* input acl' "$OUT_DIR/vpp-netns-route-plan.sh"

for ns in site-a site-b; do
  ip netns exec "$ns" ip link del "vpp-client.$VLAN_ID" 2>/dev/null || true
  ip netns exec "$ns" ip link add link vpp-client name "vpp-client.$VLAN_ID" type vlan id "$VLAN_ID"
  ip netns exec "$ns" ip link set "vpp-client.$VLAN_ID" up
done
ip netns exec site-a ip addr replace 172.16.1.2/30 dev vpp-client
ip netns exec site-b ip addr replace 172.16.2.2/30 dev vpp-client
ip netns exec site-a ip addr add 172.16.101.2/30 dev "vpp-client.$VLAN_ID"
ip netns exec site-b ip addr add 172.16.102.2/30 dev "vpp-client.$VLAN_ID"

if [ "$VPP_TABLE_ID" -gt 0 ]; then
  vppctl ip table add "$VPP_TABLE_ID" 2>/dev/null || true
fi

DRY_RUN=0 sh "$OUT_DIR/vpp-netns-route-plan.sh"

vppctl set interface ip address "host-vpp-site-a.$VLAN_ID" 172.16.101.1/30
vppctl set interface ip address "host-vpp-site-b.$VLAN_ID" 172.16.102.1/30
vppctl set interface ip address host-vpp-site-a 172.16.1.1/30
vppctl set interface ip address host-vpp-site-b 172.16.2.1/30
vppctl show interface > "$OUT_DIR/show-interface.txt"
grep -q "host-vpp-site-a.$VLAN_ID" "$OUT_DIR/show-interface.txt"
grep -q "host-vpp-site-b.$VLAN_ID" "$OUT_DIR/show-interface.txt"

for ns in site-a site-b; do
  ip netns exec "$ns" ip link add lan0 type dummy 2>/dev/null || true
  ip netns exec "$ns" ip addr flush dev lan0
  ip netns exec "$ns" ip link set lan0 up
done
ip netns exec site-a ip addr add 10.10.1.1/24 dev lan0
ip netns exec site-b ip addr add 10.10.2.1/24 dev lan0
ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.101.1 dev "vpp-client.$VLAN_ID"
ip netns exec site-b ip route replace 10.10.1.0/24 via 172.16.102.1 dev "vpp-client.$VLAN_ID"
vppctl show ip fib > "$OUT_DIR/show-ip-fib.txt"
grep -q '10.10.2.0/24' "$OUT_DIR/show-ip-fib.txt"
grep -q '10.10.1.0/24' "$OUT_DIR/show-ip-fib.txt"
if [ "$VPP_TABLE_ID" -gt 0 ]; then
  grep -q "ipv4-VRF:$VPP_TABLE_ID" "$OUT_DIR/show-ip-fib.txt"
fi
check_fib_route 10.10.2.0/24 172.16.101.2 "$VPP_TABLE_ID"
check_fib_route 10.10.1.0/24 172.16.102.2 "$VPP_TABLE_ID"

printf '== VLAN %s tagged VPP netns smoke: site-a -> site-b ==\n' "$VLAN_ID"
ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1
ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.1.1 dev vpp-client
if ip netns exec site-a ping -c 1 -W 1 -I 10.10.1.1 10.10.2.1 >/dev/null 2>&1; then
  printf '%s\n' 'untagged VLAN traffic unexpectedly passed the parent-interface ACL' >&2
  exit 1
fi
ip netns exec site-a ip route del 10.10.2.0/24 via 172.16.1.1 dev vpp-client 2>/dev/null || true
printf '\nVPP VLAN netns smoke passed: VLAN %s carried LAN traffic through VPP and routes appeared in the FIB.\n' "$VLAN_ID"
