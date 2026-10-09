#!/usr/bin/env sh
set -eu

ACTION=${1:-}
[ "$#" -gt 0 ] && shift
case "$ACTION" in
  setup|smoke|clean) ;;
  *) printf 'Usage: sh scripts/vm-netns.sh setup|smoke|clean\n' >&2; exit 2 ;;
esac

setup_runtime() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

cleanup_link() {
  ip link del "$1" 2>/dev/null || true
}

create_ns() {
  ns="$1"
  ip netns add "$ns" 2>/dev/null || true
  ip netns exec "$ns" ip link set lo up
}

create_veth_pair() {
  left_ns="$1"
  left_if="$2"
  left_ip="$3"
  right_ns="$4"
  right_if="$5"
  right_ip="$6"

  cleanup_link "$left_if"
  ip link add "$left_if" type veth peer name "$right_if"
  ip link set "$left_if" netns "$left_ns"
  ip link set "$right_if" netns "$right_ns"
  ip netns exec "$left_ns" ip addr add "$left_ip" dev "$left_if"
  ip netns exec "$right_ns" ip addr add "$right_ip" dev "$right_if"
  ip netns exec "$left_ns" ip link set "$left_if" up
  ip netns exec "$right_ns" ip link set "$right_if" up
}

for ns in site-a site-b hub-1 relay-c; do
  create_ns "$ns"
done

create_veth_pair site-a a-direct 203.0.113.10/30 site-b b-direct 203.0.113.9/30
create_veth_pair site-a a-hub 203.0.113.14/30 hub-1 hub-a 203.0.113.13/30
create_veth_pair hub-1 hub-b 203.0.113.17/30 site-b b-hub 203.0.113.18/30
create_veth_pair site-a a-relay 203.0.113.22/30 relay-c relay-a 203.0.113.21/30
create_veth_pair relay-c relay-b 203.0.113.25/30 site-b b-relay 203.0.113.26/30

ip netns exec hub-1 sysctl -w net.ipv4.ip_forward=1 >/dev/null
ip netns exec relay-c sysctl -w net.ipv4.ip_forward=1 >/dev/null

cat <<'MSG'
Created namespaces:
  site-a
  site-b
  hub-1
  relay-c

Basic checks:
  sudo ip netns exec site-a ping -c 1 203.0.113.9
  sudo ip netns exec site-a ping -c 1 203.0.113.13
  sudo ip netns exec site-b ping -c 1 203.0.113.17

Note:
  This sets up L3 namespace links only. Running strongSwan and VPP inside
  namespaces needs per-namespace daemon/process wiring, which is the next step.
MSG
)

smoke_runtime() (
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

replace_route() {
  ns="$1"
  prefix="$2"
  via="$3"

  ip netns exec "$ns" ip route replace "$prefix" via "$via"
}

delete_route() {
  ns="$1"
  prefix="$2"

  ip netns exec "$ns" ip route del "$prefix" 2>/dev/null || true
}

for ns in site-a site-b hub-1 relay-c; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
    exit 1
  }
done

ensure_dummy_lan site-a 10.10.1.1/24
ensure_dummy_lan site-b 10.10.2.1/24

printf '== direct path ==\n'
replace_route site-a 10.10.2.0/24 203.0.113.9
replace_route site-b 10.10.1.0/24 203.0.113.10
ip netns exec site-a ping -c 2 -I 10.10.1.1 10.10.2.1

printf '\n== hub path ==\n'
replace_route site-a 10.10.2.0/24 203.0.113.13
replace_route site-b 10.10.1.0/24 203.0.113.17
ip netns exec hub-1 ip route replace 10.10.1.0/24 via 203.0.113.14
ip netns exec hub-1 ip route replace 10.10.2.0/24 via 203.0.113.18
ip netns exec site-a ping -c 2 -I 10.10.1.1 10.10.2.1

printf '\n== relay path ==\n'
replace_route site-a 10.10.2.0/24 203.0.113.21
replace_route site-b 10.10.1.0/24 203.0.113.25
ip netns exec relay-c ip route replace 10.10.1.0/24 via 203.0.113.22
ip netns exec relay-c ip route replace 10.10.2.0/24 via 203.0.113.26
ip netns exec site-a ping -c 2 -I 10.10.1.1 10.10.2.1

printf '\nSmoke test passed: direct, hub, and relay L3 paths are reachable.\n'

delete_route site-a 10.10.2.0/24
delete_route site-b 10.10.1.0/24
)

clean_runtime() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

stop_namespace_processes() {
  ns="$1"
  pids=$(ip netns pids "$ns" 2>/dev/null || true)
  [ -n "$pids" ] || return 0
  kill $pids 2>/dev/null || true
  sleep 1
  pids=$(ip netns pids "$ns" 2>/dev/null || true)
  [ -n "$pids" ] && kill -KILL $pids 2>/dev/null || true
}

for ns in client-a client-b site-a site-b hub-1 relay-c; do
  stop_namespace_processes "$ns"
  ip netns del "$ns" 2>/dev/null || true
done

printf 'Removed eventnet test namespaces.\n'
)

case "$ACTION" in
  setup) setup_runtime "$@" ;;
  smoke) smoke_runtime "$@" ;;
  clean) clean_runtime "$@" ;;
esac
