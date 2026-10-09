#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_BASE=${VPP_NS_RUN_BASE:-/run/ibuki-vpp-ns}
VPPCTL=${VPPCTL:-vppctl}
VPPCTL_TIMEOUT=${VPPCTL_TIMEOUT:-0}
VPP_TOPOLOGY_MODE=${VPP_TOPOLOGY_MODE:-all}
ACTION=${1:-}

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s setup|clean|status\n' "$0" >&2
  exit 1
fi

vpp() {
  local ns socket command_text output status
  ns="$1"
  shift
  command_text="$*"
  socket="$RUN_BASE/$ns/cli.sock"
  if [ ! -S "$socket" ]; then
    printf 'VPP CLI socket is unavailable for %s: %s\n' "$ns" "$socket" >&2
    return 1
  fi
  if [ "$VPPCTL_TIMEOUT" -gt 0 ] && command -v timeout >/dev/null 2>&1; then
    if output=$(timeout "$VPPCTL_TIMEOUT" "$VPPCTL" -s "$socket" "$@" 2>&1); then
      status=0
    else
      status=$?
    fi
  elif output=$("$VPPCTL" -s "$socket" "$@" 2>&1); then
    status=0
  else
    status=$?
  fi
  [ -z "$output" ] || printf '%s\n' "$output"
  if [ "$status" -ne 0 ] || printf '%s\n' "$output" | grep -Eq '(^|[[:space:]])(Error:|unknown input|failed|invalid|connect:)'; then
    printf 'VPP command failed in %s: %s\n' "$ns" "$command_text" >&2
    return 1
  fi
}

disable_veth_offloads() {
  local ns interface
  ns="$1"
  interface="$2"
  if command -v ethtool >/dev/null 2>&1; then
    ip netns exec "$ns" ethtool -K "$interface" \
      rx off tx off tso off gso off gro off lro off >/dev/null 2>&1 || true
  fi
}

link_pair() {
  local ns vpp_if linux_if linux_addr
  ns="$1"
  vpp_if="$2"
  linux_if="$3"
  linux_addr="$4"
  ip netns exec "$ns" ip link del "$vpp_if" >/dev/null 2>&1 || true
  ip netns exec "$ns" ip link add "$vpp_if" type veth peer name "$linux_if"
  ip netns exec "$ns" ip link set "$vpp_if" up
  ip netns exec "$ns" ip addr add "$linux_addr" dev "$linux_if"
  ip netns exec "$ns" ip link set "$linux_if" up
  disable_veth_offloads "$ns" "$vpp_if"
  disable_veth_offloads "$ns" "$linux_if"
}

configure_site() {
  local ns client_ns vpp_lan client_lan_addr vpp_lan_addr vpp_lan_ip vpp_underlay linux_underlay linux_underlay_addr vpp_underlay_addr
  ns="$1"; client_ns="$2"; vpp_lan="$3"; client_lan_addr="$4"; vpp_lan_addr="$5"
  vpp_underlay="$6"; linux_underlay="$7"; linux_underlay_addr="$8"; vpp_underlay_addr="$9"
  vpp "$ns" delete host-interface name "$vpp_lan" >/dev/null 2>&1 || true
  vpp "$ns" delete host-interface name "$vpp_underlay" >/dev/null 2>&1 || true
  ip netns del "$client_ns" >/dev/null 2>&1 || true
  ip netns add "$client_ns"
  ip netns exec "$ns" ip link del "$vpp_lan" >/dev/null 2>&1 || true
  ip netns exec "$ns" ip link add "$vpp_lan" type veth peer name eth0
  ip netns exec "$ns" ip link set eth0 netns "$client_ns"
  ip netns exec "$ns" ip link set "$vpp_lan" up
  ip netns exec "$client_ns" ip link set lo up
  ip netns exec "$client_ns" ip addr add "$client_lan_addr" dev eth0
  ip netns exec "$client_ns" ip link set eth0 up
  disable_veth_offloads "$ns" "$vpp_lan"
  disable_veth_offloads "$client_ns" eth0
  link_pair "$ns" "$vpp_underlay" "$linux_underlay" "$linux_underlay_addr"
  vpp "$ns" create host-interface name "$vpp_lan" cksum-gso-disable
  vpp "$ns" set interface state "host-$vpp_lan" up
  vpp "$ns" set interface ip address "host-$vpp_lan" "$vpp_lan_addr"
  vpp_lan_ip=${vpp_lan_addr%/*}
  ip netns exec "$client_ns" ip route replace default via "$vpp_lan_ip"
  vpp "$ns" create host-interface name "$vpp_underlay" cksum-gso-disable
  vpp "$ns" set interface state "host-$vpp_underlay" up
  vpp "$ns" set interface ip address "host-$vpp_underlay" "$vpp_underlay_addr"
}

create_cross_namespace_veth() {
  local left_ns left_if right_ns right_if
  left_ns="$1"
  left_if="$2"
  right_ns="$3"
  right_if="$4"

  ip netns exec "$left_ns" ip link del "$left_if" >/dev/null 2>&1 || true
  ip netns exec "$right_ns" ip link del "$right_if" >/dev/null 2>&1 || true
  ip netns exec "$left_ns" ip link add "$left_if" type veth peer name "$right_if"
  ip netns exec "$left_ns" ip link set "$right_if" netns "$right_ns"
  ip netns exec "$left_ns" ip link set "$left_if" up
  ip netns exec "$right_ns" ip link set "$right_if" up
  disable_veth_offloads "$left_ns" "$left_if"
  disable_veth_offloads "$right_ns" "$right_if"
}

configure_vpp_transit_link() {
  local left_ns left_if left_addr right_ns right_if right_addr
  left_ns="$1"
  left_if="$2"
  left_addr="$3"
  right_ns="$4"
  right_if="$5"
  right_addr="$6"

  vpp "$left_ns" delete host-interface name "$left_if" >/dev/null 2>&1 || true
  vpp "$right_ns" delete host-interface name "$right_if" >/dev/null 2>&1 || true
  create_cross_namespace_veth "$left_ns" "$left_if" "$right_ns" "$right_if"
  vpp "$left_ns" create host-interface name "$left_if" cksum-gso-disable
  vpp "$right_ns" create host-interface name "$right_if" cksum-gso-disable
  vpp "$left_ns" set interface state "host-$left_if" up
  vpp "$right_ns" set interface state "host-$right_if" up
  vpp "$left_ns" set interface ip address "host-$left_if" "$left_addr"
  vpp "$right_ns" set interface ip address "host-$right_if" "$right_addr"
}

configure_hub_vpp_links() {
  configure_vpp_transit_link site-a ib-ah 172.17.1.1/30 hub-1 ib-ha 172.17.1.2/30
  configure_vpp_transit_link hub-1 ib-hb 172.17.2.1/30 site-b ib-bh 172.17.2.2/30

  vpp hub-1 ip route del 10.10.1.0/24 >/dev/null 2>&1 || true
  vpp hub-1 ip route del 10.10.2.0/24 >/dev/null 2>&1 || true
  vpp hub-1 ip route add 10.10.1.0/24 via 172.17.1.1 host-ib-ha
  vpp hub-1 ip route add 10.10.2.0/24 via 172.17.2.2 host-ib-hb
  vpp site-a ip route del 10.10.2.0/24 >/dev/null 2>&1 || true
  vpp site-b ip route del 10.10.1.0/24 >/dev/null 2>&1 || true
  vpp site-a ip route add 10.10.2.0/24 via 172.17.1.2 host-ib-ah
  vpp site-b ip route add 10.10.1.0/24 via 172.17.2.1 host-ib-bh

  printf 'Configured VPP Hub links: site-a <-> hub-1 <-> site-b.\n'
}

configure_direct_vpp_links() {
  configure_vpp_transit_link site-a ib-dir-a 172.18.1.1/30 site-b ib-dir-b 172.18.1.2/30
  vpp site-a ip route del 10.10.2.0/24 >/dev/null 2>&1 || true
  vpp site-b ip route del 10.10.1.0/24 >/dev/null 2>&1 || true
  vpp site-a ip route add 10.10.2.0/24 via 172.18.1.2 host-ib-dir-a
  vpp site-b ip route add 10.10.1.0/24 via 172.18.1.1 host-ib-dir-b
  printf 'Configured direct VPP links: site-a <-> site-b.\n'
}

configure_ipsec_tap() {
  local ns linux_if
  ns="$1"
  linux_if="$2"

  if ! ip netns exec "$ns" ip link show "$linux_if" >/dev/null 2>&1; then
    vpp "$ns" create tap id 0 host-if-name "$linux_if"
  fi
  vpp "$ns" set interface state tap0 up
  vpp "$ns" set interface ip address del tap0 169.254.100.1/30 >/dev/null 2>&1 || true
  vpp "$ns" set interface ip address tap0 169.254.100.1/30
  ip netns exec "$ns" ip addr replace 169.254.100.2/30 dev "$linux_if"
  ip netns exec "$ns" ip link set "$linux_if" up
  disable_veth_offloads "$ns" "$linux_if"
}

configure_ipsec_taps() {
  configure_ipsec_tap site-a ib-ipsec-a
  configure_ipsec_tap site-b ib-ipsec-b
  ip netns exec site-a ip route replace 10.10.1.0/24 via 169.254.100.1 dev ib-ipsec-a
  ip netns exec site-b ip route replace 10.10.2.0/24 via 169.254.100.1 dev ib-ipsec-b
  printf 'Configured VPP-to-Linux IPsec TAP links for site-a and site-b.\n'
}

setup() {
  for ns in site-a site-b; do
    ip netns exec "$ns" true >/dev/null 2>&1 || { printf 'namespace missing: %s\n' "$ns" >&2; exit 1; }
  done
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" start
  ip netns exec site-a ip link del lan0 >/dev/null 2>&1 || true
  ip netns exec site-b ip link del lan0 >/dev/null 2>&1 || true
  configure_site site-a client-a ib-lan-a 10.10.1.2/24 10.10.1.1/24 ib-ul-a ib-ul-a-peer 198.18.1.2/30 198.18.1.1/30
  configure_site site-b client-b ib-lan-b 10.10.2.2/24 10.10.2.1/24 ib-ul-b ib-ul-b-peer 198.18.2.2/30 198.18.2.1/30
  case "$VPP_TOPOLOGY_MODE" in
    hub) configure_hub_vpp_links ;;
    direct) configure_direct_vpp_links ;;
    ipsec) printf 'Using Linux IPsec underlay; skipping plaintext VPP transit links.\n' ;;
    all) configure_hub_vpp_links; configure_direct_vpp_links ;;
    *) printf 'invalid VPP_TOPOLOGY_MODE: %s\n' "$VPP_TOPOLOGY_MODE" >&2; exit 2 ;;
  esac
  if [ "${SKIP_IPSEC_TAP:-0}" != "1" ]; then
    configure_ipsec_taps
  else
    printf 'Skipping VPP-to-Linux IPsec TAP links.\n'
  fi
  ip netns exec site-a sysctl -w net.ipv4.ip_forward=1 >/dev/null
  ip netns exec site-b sysctl -w net.ipv4.ip_forward=1 >/dev/null
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
  vpp site-a ip route add 10.10.2.0/24 via 172.18.1.2 host-ib-dir-a
  vpp site-b ip route add 10.10.1.0/24 via 172.18.1.1 host-ib-dir-b
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
  for spec in "site-a ib-ah" "hub-1 ib-ha" "hub-1 ib-hb" "site-b ib-bh" "site-a ib-dir-a" "site-b ib-dir-b"; do
    set -- $spec
    vpp "$1" delete host-interface name "$2" >/dev/null 2>&1 || true
  done
  for spec in "site-a ib-ah" "hub-1 ib-ha" "hub-1 ib-hb" "site-b ib-bh" "site-a ib-dir-a" "site-b ib-dir-b" "site-a ib-ipsec-a" "site-b ib-ipsec-b"; do
    set -- $spec
    ip netns exec "$1" ip link del "$2" >/dev/null 2>&1 || true
  done
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" stop >/dev/null 2>&1 || true
  ip netns del client-a >/dev/null 2>&1 || true
  ip netns del client-b >/dev/null 2>&1 || true
}

status() {
  sh "$ROOT_DIR/scripts/vm-vpp-ns-runtime.sh" status
  for ns in site-a site-b client-a client-b; do
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
