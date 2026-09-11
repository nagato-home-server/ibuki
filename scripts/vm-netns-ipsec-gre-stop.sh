#!/usr/bin/env sh
set -eu

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

RUN_BASE="${GRE_RUN_BASE:-/run/eventnet-netns-ipsec-gre}"
for ns in site-a site-b; do
  run_dir="$RUN_BASE/$ns"
  if [ -s "$run_dir/charon.pid" ]; then kill "$(cat "$run_dir/charon.pid")" 2>/dev/null || true; fi
  if [ -s "$run_dir/eventnet-wrapper.pid" ]; then kill "$(cat "$run_dir/eventnet-wrapper.pid")" 2>/dev/null || true; fi
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
  rm -f "$run_dir/charon.pid" "$run_dir/eventnet-wrapper.pid" "$run_dir/charon.vici"
done
printf 'Stopped GRE charon processes and flushed GRE XFRM state.\n'
