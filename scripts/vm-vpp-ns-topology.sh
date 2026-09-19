#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_BASE=${VPP_NS_RUN_BASE:-/run/ibuki-vpp-ns}
VPPCTL=${VPPCTL:-vppctl}
ACTION=${1:-}

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s setup|clean|status\n' "$0" >&2
  exit 1
fi

vpp() {
  ns="$1"
  shift
  "$VPPCTL" -s "$RUN_BASE/$ns/cli.sock" "$*"
}

link_pair() {
  ns="$1"
  vpp_if="$2"
  linux_if="$3"
  linux_addr="$4"
  ip netns exec "$ns" ip link del "$vpp_if" >/dev/null 2>&1 || true
  ip netns exec "$ns" ip link add "$vpp_if" type veth peer name "$linux_if"
  ip netns exec "$ns" ip link set "$vpp_if" up
  ip netns exec "$ns" ip addr add "$linux_addr" dev "$linux_if"
  ip netns exec "$ns" ip link set "$linux_if" up
}

configure_site() {
  ns="$1"; vpp_lan="$2"; linux_lan="$3"; linux_lan_addr="$4"; vpp_lan_addr="$5"
  vpp_underlay="$6"; linux_underlay="$7"; linux_underlay_addr="$8"; vpp_underlay_addr="$9"
  vpp "$ns" delete host-interface name "$vpp_lan" >/dev/null 2>&1 || true
  vpp "$ns" delete host-interface name "$vpp_underlay" >/dev/null 2>&1 || true
  link_pair "$ns" "$vpp_lan" "$linux_lan" "$linux_lan_addr"
  link_pair "$ns" "$vpp_underlay" "$linux_underlay" "$linux_underlay_addr"
  vpp "$ns" create host-interface name "$vpp_lan"
  vpp "$ns" set interface state "host-$vpp_lan" up
  vpp "$ns" set interface ip address "host-$vpp_lan" "$vpp_lan_addr"
  vpp "$ns" create host-interface name "$vpp_underlay"
  vpp "$ns" set interface state "host-$vpp_underlay" up
  vpp "$ns" set interface ip address "host-$vpp_underlay" "$vpp_underlay_addr"
}

setup() {
  for ns in site-a site-b; do
    ip netns exec "$ns" true >/dev/null 2>&1 || { printf 'namespace missing: %s\n' "$ns" >&2; exit 1; }
  done
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" start
  configure_site site-a ib-lan-a ib-lan-a-peer 172.16.1.2/30 172.16.1.1/30 ib-ul-a ib-ul-a-peer 198.18.1.2/30 198.18.1.1/30
  configure_site site-b ib-lan-b ib-lan-b-peer 172.16.2.2/30 172.16.2.1/30 ib-ul-b ib-ul-b-peer 198.18.2.2/30 198.18.2.1/30
  ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.1.1 dev ib-lan-a-peer
  ip netns exec site-b ip route replace 10.10.1.0/24 via 172.16.2.1 dev ib-lan-b-peer
  ip netns exec site-a ip route replace 198.18.2.1/32 via 203.0.113.9 dev a-direct
  ip netns exec site-b ip route replace 198.18.1.1/32 via 203.0.113.10 dev b-direct
  vpp site-a ip route add 198.18.2.1/32 via 198.18.1.2 host-ib-ul-a
  vpp site-b ip route add 198.18.1.1/32 via 198.18.2.2 host-ib-ul-b
  if [ "${SKIP_GRE:-0}" != "1" ]; then
  vpp site-a create gre tunnel src 198.18.1.1 dst 198.18.2.1 instance 0 del >/dev/null 2>&1 || true
  vpp site-b create gre tunnel src 198.18.2.1 dst 198.18.1.1 instance 0 del >/dev/null 2>&1 || true
  vpp site-a create gre tunnel src 198.18.1.1 dst 198.18.2.1 instance 0
  vpp site-b create gre tunnel src 198.18.2.1 dst 198.18.1.1 instance 0
  vpp site-a set interface ip address gre0 10.255.0.1/30
  vpp site-b set interface ip address gre0 10.255.0.2/30
  vpp site-a set interface state gre0 up
  vpp site-b set interface state gre0 up
  vpp site-a ip route add 10.10.2.0/24 via 10.255.0.2 gre0
  vpp site-b ip route add 10.10.1.0/24 via 10.255.0.1 gre0
  else
    printf 'Configured namespaced VPP topology: LAN and underlay only.\n'
  fi
}

clean() {
  for ns in site-a site-b; do
    vpp "$ns" delete host-interface name ib-lan-a >/dev/null 2>&1 || true
    vpp "$ns" delete host-interface name ib-lan-b >/dev/null 2>&1 || true
    vpp "$ns" delete host-interface name ib-ul-a >/dev/null 2>&1 || true
    vpp "$ns" delete host-interface name ib-ul-b >/dev/null 2>&1 || true
    ip netns exec "$ns" ip link del ib-lan-a >/dev/null 2>&1 || true
    ip netns exec "$ns" ip link del ib-lan-b >/dev/null 2>&1 || true
    ip netns exec "$ns" ip link del ib-ul-a >/dev/null 2>&1 || true
    ip netns exec "$ns" ip link del ib-ul-b >/dev/null 2>&1 || true
  done
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" stop >/dev/null 2>&1 || true
}

status() {
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" status
  for ns in site-a site-b; do
    printf '\n== %s interfaces ==\n' "$ns"
    ip netns exec "$ns" ip -br addr
    vpp "$ns" show interface 2>/dev/null || true
  done
}

case "$ACTION" in
  setup) setup ;;
  clean) clean ;;
  status) status ;;
  *) printf 'Usage: sudo %s setup|clean|status\n' "$0" >&2; exit 2 ;;
esac
