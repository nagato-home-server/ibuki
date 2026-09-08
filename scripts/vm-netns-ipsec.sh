#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
  cat >&2 <<'EOF'
Usage:
  sh scripts/vm-netns-ipsec.sh direct|hub generate
  sudo sh scripts/vm-netns-ipsec.sh direct|hub start
  sudo sh scripts/vm-netns-ipsec.sh direct|hub status
  sh scripts/vm-netns-ipsec.sh direct|hub logs
  sudo sh scripts/vm-netns-ipsec.sh direct|hub smoke
  sudo sh scripts/vm-netns-ipsec.sh direct|hub stop
  sudo sh scripts/vm-netns-ipsec.sh direct|hub clean
EOF
  exit 1
}

need_root() {
  if [ "$(id -u)" != "0" ]; then
    printf 'Please run as root: sudo %s %s %s\n' "$0" "$MODE" "$ACTION" >&2
    exit 1
  fi
}

MODE="${1:-}"
ACTION="${2:-}"

[ -n "$MODE" ] && [ -n "$ACTION" ] || usage

case "$MODE" in
  direct)
    NODES="site-a site-b"
    RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
    SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-direct}"
    ;;
  hub)
    NODES="site-a hub-1 site-b"
    RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"
    SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-hub}"
    ;;
  *)
    usage
    ;;
esac

run_existing() {
  sh "$ROOT_DIR/scripts/vm-netns-ipsec-$MODE-$1.sh"
}

clean_direct() {
  need_root
  for ns in site-a site-b; do
    ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
    ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
  done
  printf 'Flushed xfrm state and policy in site-a/site-b.\n'
}

clean_hub() {
  need_root
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
}

show_logs() {
  lines=120
  [ "$MODE" = "hub" ] && lines=160
  for ns in $NODES; do
    printf '\n== %s charon.log ==\n' "$ns"
    if [ -f "$RUN_BASE/$ns/charon.log" ]; then
      tail -n "$lines" "$RUN_BASE/$ns/charon.log"
    else
      printf 'missing: %s\n' "$RUN_BASE/$ns/charon.log"
    fi
  done
}

show_status() {
  need_root
  for ns in $NODES; do
    printf '\n== %s ==\n' "$ns"
    ip netns exec "$ns" ip addr show
    if [ "$MODE" = "hub" ]; then
      printf '\n-- routes --\n'
      ip netns exec "$ns" ip route
      printf '\n-- xfrm links --\n'
      ip netns exec "$ns" ip -d link show type xfrm || true
    fi
    printf '\n-- xfrm state --\n'
    ip netns exec "$ns" ip xfrm state || true
    printf '\n-- xfrm policy --\n'
    ip netns exec "$ns" ip xfrm policy || true
    if [ -s "$RUN_BASE/$ns/charon.pid" ]; then
      printf '\ncharon pid: %s\n' "$(cat "$RUN_BASE/$ns/charon.pid")"
    fi
    if [ -f "$SWANCTL_WORK_BASE/$ns/swanctl.conf" ]; then
      printf 'swanctl work config: %s\n' "$SWANCTL_WORK_BASE/$ns/swanctl.conf"
      ls -ld "$SWANCTL_WORK_BASE" "$SWANCTL_WORK_BASE/$ns" "$SWANCTL_WORK_BASE/$ns/swanctl.conf"
    fi
    printf '\n-- swanctl conns --\n'
    swanctl --list-conns --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null || true
    printf '\n-- swanctl sas --\n'
    swanctl --list-sas --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null || true
  done
}

case "$ACTION" in
  generate|start|smoke|stop)
    run_existing "$ACTION"
    ;;
  clean)
    if [ "$MODE" = "direct" ]; then
      clean_direct
    else
      clean_hub
    fi
    ;;
  logs)
    show_logs
    ;;
  status)
    show_status
    ;;
  *)
    usage
    ;;
esac
