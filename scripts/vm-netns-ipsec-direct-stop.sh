#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-direct}"
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-direct}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a site-b; do
  pid_file="$RUN_BASE/$ns/charon.pid"
  wrapper_pid_file="$RUN_BASE/$ns/eventnet-wrapper.pid"
  if [ -s "$pid_file" ]; then
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
      printf 'Stopping %s charon pid %s\n' "$ns" "$pid"
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
for ns in site-a site-b; do
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
done

printf 'Flushed xfrm state and policy in site-a/site-b.\n'
