#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML="${1:-samples/linux-vm-netns.yaml}"

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

for ns in site-a site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
    exit 1
  }
done

ensure_dummy_lan site-a 10.10.1.1/24
ensure_dummy_lan site-b 10.10.2.1/24

sh scripts/vm-generate-netns-runtime.sh "$YAML"
grep -q '^selected_path: path-direct$' out/netns-runtime/selected-path.txt

sh scripts/vm-vpp-netns.sh setup
DRY_RUN=0 sh out/netns-runtime/vpp-netns-route-plan.sh

ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.1.1
ip netns exec site-b ip route replace 10.10.1.0/24 via 172.16.2.1

printf '== controller VPP netns LAN smoke: site-a -> site-b ==\n'
ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1

printf '\n== controller VPP netns LAN smoke: site-b -> site-a ==\n'
ip netns exec site-b ping -c 3 -I 10.10.2.1 10.10.1.1

printf '\nController VPP netns smoke passed: controller-generated VPP routes carried LAN traffic.\n'
