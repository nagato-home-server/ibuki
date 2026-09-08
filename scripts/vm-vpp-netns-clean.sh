#!/usr/bin/env sh
set -eu

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
