#!/usr/bin/env sh
set -eu

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
