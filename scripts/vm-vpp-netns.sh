#!/usr/bin/env sh
set -eu

ACTION=${1:-}
[ "$#" -gt 0 ] && shift
case "$ACTION" in
  setup|status|clean) ;;
  *) printf 'Usage: sh scripts/vm-vpp-netns.sh setup|status|clean\n' >&2; exit 2 ;;
esac

setup_runtime() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'missing: %s\n' "$1" >&2
    exit 1
  fi
}

need_cmd ip
need_cmd vppctl
VPP_TOPOLOGY="${VPP_TOPOLOGY:-edge}"
case "$VPP_TOPOLOGY" in
  edge|hub) ;;
  *) printf 'invalid VPP_TOPOLOGY: %s\n' "$VPP_TOPOLOGY" >&2; exit 2 ;;
esac

for ns in site-a site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
    exit 1
  }
done
if [ "$VPP_TOPOLOGY" = "hub" ]; then
  ip netns exec hub-1 true >/dev/null 2>&1 || {
    printf 'namespace missing: hub-1. Run sudo sh scripts/vm-netns.sh setup first.\n' >&2
    exit 1
  }
fi

sh "$(dirname -- "$0")/vm-vpp-netns.sh" clean

create_vpp_veth() {
  ns="$1"
  host_if="$2"
  ns_if="$3"
  ns_addr="$4"
  vpp_addr="$5"

  ip link add "$host_if" type veth peer name "$ns_if"
  ip link set "$ns_if" netns "$ns"
  ip link set "$host_if" up
  ip netns exec "$ns" ip addr flush dev "$ns_if"
  ip netns exec "$ns" ip addr add "$ns_addr" dev "$ns_if"
  ip netns exec "$ns" ip link set "$ns_if" up

  vppctl create host-interface name "$host_if" >/dev/null
  vppctl set interface state "host-$host_if" up
  vppctl set interface ip address "host-$host_if" "$vpp_addr"
}

create_vpp_veth site-a vpp-site-a vpp-client 172.16.1.2/30 172.16.1.1/30
create_vpp_veth site-b vpp-site-b vpp-client 172.16.2.2/30 172.16.2.1/30

if [ "$VPP_TOPOLOGY" = "hub" ]; then
  create_vpp_veth hub-1 vpp-hub-a vpp-client-a 172.16.3.2/30 172.16.3.1/30
  create_vpp_veth hub-1 vpp-hub-b vpp-client-b 172.16.4.2/30 172.16.4.1/30
  ip netns exec hub-1 sysctl -w net.ipv4.ip_forward=1 >/dev/null
  ip netns exec hub-1 ip route replace 10.10.1.0/24 via 172.16.3.1
  ip netns exec hub-1 ip route replace 10.10.2.0/24 via 172.16.4.1
fi

ip netns exec site-a ip route replace 172.16.2.0/30 via 172.16.1.1
ip netns exec site-b ip route replace 172.16.1.0/30 via 172.16.2.1

if [ "$VPP_TOPOLOGY" = "hub" ]; then
  cat <<'MSG'
Created VPP Hub netns links:
  site-a:vpp-client 172.16.1.2/30 <-> VPP host-vpp-site-a 172.16.1.1/30
  hub-1:vpp-client-a 172.16.3.2/30 <-> VPP host-vpp-hub-a 172.16.3.1/30
  hub-1:vpp-client-b 172.16.4.2/30 <-> VPP host-vpp-hub-b 172.16.4.1/30
  site-b:vpp-client 172.16.2.2/30 <-> VPP host-vpp-site-b 172.16.2.1/30
MSG
else
  cat <<'MSG'
Created VPP netns links:
  site-a:vpp-client 172.16.1.2/30 <-> VPP host-vpp-site-a 172.16.1.1/30
  site-b:vpp-client 172.16.2.2/30 <-> VPP host-vpp-site-b 172.16.2.1/30

Next:
  sudo sh scripts/vm-vpp-netns.sh status
  sudo sh scripts/vm-vpp-netns-smoke.sh
MSG
fi
)

status_runtime() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

printf '== Linux host links ==\n'
ip addr show vpp-site-a 2>/dev/null || true
ip addr show vpp-site-b 2>/dev/null || true

for ns in site-a site-b; do
  printf '\n== %s vpp-client ==\n' "$ns"
  ip netns exec "$ns" ip addr show vpp-client 2>/dev/null || true
  printf '\n== %s routes ==\n' "$ns"
  ip netns exec "$ns" ip route 2>/dev/null || true
done

printf '\n== VPP interfaces ==\n'
vppctl show interface

printf '\n== VPP interface addresses ==\n'
vppctl show interface address

printf '\n== VPP IPv4 FIB ==\n'
vppctl show ip fib
)

clean_runtime() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a site-b hub-1; do
  ip netns exec "$ns" ip link del vpp-client 2>/dev/null || true
  ip netns exec "$ns" ip link del vpp-client-a 2>/dev/null || true
  ip netns exec "$ns" ip link del vpp-client-b 2>/dev/null || true
done

ip link del vpp-site-a 2>/dev/null || true
ip link del vpp-site-b 2>/dev/null || true
ip link del vpp-hub-a 2>/dev/null || true
ip link del vpp-hub-b 2>/dev/null || true

if command -v vppctl >/dev/null 2>&1; then
  vppctl delete host-interface name vpp-site-a 2>/dev/null || true
  vppctl delete host-interface name vpp-site-b 2>/dev/null || true
  vppctl delete host-interface name vpp-hub-a 2>/dev/null || true
  vppctl delete host-interface name vpp-hub-b 2>/dev/null || true
fi

printf 'Cleaned VPP netns veth links and host interfaces.\n'
)

case "$ACTION" in
  setup) setup_runtime "$@" ;;
  status) status_runtime "$@" ;;
  clean) clean_runtime "$@" ;;
esac
