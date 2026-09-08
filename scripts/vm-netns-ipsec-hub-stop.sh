#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-hub}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a hub-1 site-b; do
  pid_file="$RUN_BASE/$ns/charon.pid"
  wrapper_pid_file="$RUN_BASE/$ns/eventnet-wrapper.pid"
  if [ -s "$pid_file" ]; then
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
      printf 'Stopping %s hub charon pid %s\n' "$ns" "$pid"
      kill "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
  fi
  if [ -s "$wrapper_pid_file" ]; then
    wrapper_pid=$(cat "$wrapper_pid_file")
    kill "$wrapper_pid" 2>/dev/null || true
    rm -f "$wrapper_pid_file"
  fi
  rm -f "$RUN_BASE/$ns/charon.vici"
done

rm -rf "$SWANCTL_WORK_BASE"
ip netns exec site-a ip route del 10.10.2.0/24 2>/dev/null || true
ip netns exec hub-1 ip route del 10.10.1.0/24 2>/dev/null || true
ip netns exec hub-1 ip route del 10.10.2.0/24 2>/dev/null || true
ip netns exec site-b ip route del 10.10.1.0/24 2>/dev/null || true

for ns in site-a hub-1 site-b; do
  ip netns exec "$ns" ip link del xfrm-a-hub 2>/dev/null || true
  ip netns exec "$ns" ip link del xfrm-hub-b 2>/dev/null || true
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
done

printf 'Flushed hub XFRM state, policy, routes, and xfrm interfaces.\n'
