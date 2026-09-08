#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML="${1:-samples/vpp-netns-routes.yaml}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/vpp-routes-yaml-netns}"


if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

ensure_dummy_lan() {
  ns="$1"
  addr="$2"

  ip netns exec "$ns" ip link show lan0 >/dev/null 2>&1 || \
    ip netns exec "$ns" ip link add lan0 type dummy
  ip netns exec "$ns" ip addr flush dev lan0
  ip netns exec "$ns" ip addr add "$addr" dev lan0
  ip netns exec "$ns" ip link set lan0 up
}

cd "$ROOT_DIR"

check_fib_route() {
  prefix="$1"
  next_hop="$2"
  table_id="$3"
  awk -v prefix="$prefix" -v next_hop="$next_hop" -v wanted_table="$table_id" '
    /ipv4-VRF:/ {
      table = $0
      sub(/^.*ipv4-VRF:/, "", table)
      sub(/,.*/, "", table)
      next
    }
    $0 == prefix && table == wanted_table { in_route = 1; next }
    in_route && $0 ~ ("via " next_hop) { matched = 1; next }
    in_route && $0 !~ /^[[:space:]]/ { in_route = 0 }
    END { exit matched ? 0 : 1 }
  ' "$OUT_DIR/show-ip-fib.txt"
}

for ns in site-a site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns-setup.sh first.\n' "$ns" >&2
    exit 1
  }
done

ensure_dummy_lan site-a 10.10.1.1/24
ensure_dummy_lan site-b 10.10.2.1/24

OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --path path-vpp-explicit-bidirectional
grep -q '^selected_path: path-vpp-explicit-bidirectional$' "$OUT_DIR/selected-path.txt"

sh scripts/vm-vpp-netns-setup.sh
vppctl ip table add 100 2>/dev/null || true
DRY_RUN=0 sh "$OUT_DIR/vpp-netns-route-plan.sh"
vppctl show ip fib > "$OUT_DIR/show-ip-fib.txt"
grep -q 'ipv4-VRF:100' "$OUT_DIR/show-ip-fib.txt"
grep -q '10.10.2.0/24' "$OUT_DIR/show-ip-fib.txt"
grep -q '10.10.1.0/24' "$OUT_DIR/show-ip-fib.txt"
grep -q 'ensure_vpp_table 100' "$OUT_DIR/vpp-route-plan.sh"
grep -q 'ensure_vpp_table 100' "$OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'run_vpp show ip fib' "$OUT_DIR/vpp-route-plan.sh"
grep -q 'run_vpp show ip fib' "$OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ip route add 10.10.2.0/24 table 100 via 203.0.113.9' "$OUT_DIR/vpp-netns-route-plan.sh"
grep -q 'ip route add 10.10.1.0/24 table 100 via 203.0.113.10' "$OUT_DIR/vpp-netns-route-plan.sh"
check_fib_route 10.10.2.0/24 203.0.113.9 100
check_fib_route 10.10.1.0/24 203.0.113.10 100

ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.1.1
ip netns exec site-b ip route replace 10.10.1.0/24 via 172.16.2.1

printf '== YAML explicit routes VPP netns smoke: site-a -> site-b ==\n'
ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1

printf '\n== YAML explicit routes VPP netns smoke: site-b -> site-a ==\n'
ip netns exec site-b ping -c 3 -I 10.10.2.1 10.10.1.1

printf '\nYAML routes VPP netns smoke passed: explicit bidirectional routes carried LAN traffic and appeared in VPP FIB.\n'
