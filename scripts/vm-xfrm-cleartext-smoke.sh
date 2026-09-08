#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_BASE=${RUN_BASE:-/run/eventnet-netns-ipsec-direct}
SOURCE_PREFIX=${SOURCE_PREFIX:-10.10.1.0/24}
DESTINATION_PREFIX=${DESTINATION_PREFIX:-10.10.2.0/24}
BLOCK_PRIORITY=${BLOCK_PRIORITY:-10000}

if [ "$(id -u)" != "0" ]; then
  printf '%s\n' 'Please run as root: sudo sh scripts/vm-xfrm-cleartext-smoke.sh' >&2
  exit 1
fi
cd "$ROOT_DIR"

cleanup() {
  sh scripts/vm-netns-ipsec-direct-stop.sh >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

apply_block() {
  ip netns exec site-a ip xfrm policy add dir out src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" action block
  ip netns exec site-a ip xfrm policy add dir in src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" action block
  ip netns exec site-b ip xfrm policy add dir out src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" action block
  ip netns exec site-b ip xfrm policy add dir in src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" action block
}

sh scripts/vm-netns-ipsec-hub-stop.sh >/dev/null 2>&1 || true
sh scripts/vm-netns-ipsec-direct-start.sh
sh scripts/vm-netns-ipsec-direct-smoke.sh
apply_block
printf '%s\n' '== IPsec remains usable with lower-priority cleartext block =='
sh scripts/vm-netns-ipsec-direct-smoke.sh

sh scripts/vm-netns-ipsec-direct-stop.sh
ip netns exec site-a ip route replace "$DESTINATION_PREFIX" via 203.0.113.9
ip netns exec site-b ip route replace "$SOURCE_PREFIX" via 203.0.113.10
apply_block

printf '%s\n' '== cleartext must be blocked after SA removal =='
if ip netns exec site-a ping -c 1 -W 1 -I 10.10.1.1 10.10.2.1 >/dev/null 2>&1; then
  printf '%s\n' 'cleartext ping unexpectedly succeeded after IPsec SA removal' >&2
  exit 1
fi
printf '%s\n' 'cleartext_ping: blocked'

printf '%s\n' 'XFRM cleartext smoke passed: encrypted traffic worked and cleartext was blocked after SA removal.'
