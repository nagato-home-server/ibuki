#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
YAML=${1:-samples/linux-vm-netns.yaml}
ACTION=${2:-all}
KEEP_RUNTIME=${KEEP_RUNTIME:-0}

usage() {
  cat >&2 <<EOF
Usage:
  sudo sh scripts/vm-real-smoke.sh [yaml] [action]

Actions:
  preflight     Check commands, namespaces, VPP and strongSwan prerequisites.
  underlay      Recreate namespaces and verify direct/hub/relay L3 paths.
  vpp           Start namespaced VPP and verify bidirectional forwarding.
  ipsec         Start direct strongSwan and verify encrypted LAN traffic.
  gre            Run namespaced GRE over IPsec smoke using the controller plan.
  integrated    Run controller-generated IPsec + VPP runtime in both modes.
  all           Run preflight, underlay, VPP, IPsec and GRE checks.
  clean         Stop daemons, remove VPP topology and clean namespaces.

Environment:
  KEEP_RUNTIME=1  Keep runtime state after a successful test.
EOF
  exit 2
}

case "$ACTION" in
  preflight|underlay|vpp|ipsec|gre|integrated|all|clean) ;;
  *) usage ;;
esac

cd "$ROOT_DIR"

run() {
  label="$1"
  shift
  printf '\n===== %s =====\n' "$label"
  "$@"
}

require_root() {
  if [ "$(id -u)" != "0" ]; then
    printf 'Run this action as root: sudo sh scripts/vm-real-smoke.sh %s %s\n' "$YAML" "$ACTION" >&2
    exit 1
  fi
}

ensure_underlay() {
  if ! ip netns exec site-a true >/dev/null 2>&1 ||
     ! ip netns exec site-b true >/dev/null 2>&1; then
    run 'namespace setup' sh scripts/vm-netns.sh setup
  fi
}

cleanup_runtime() {
  [ "$KEEP_RUNTIME" = "1" ] && return 0
  sh scripts/vm-netns-ipsec.sh direct stop >/dev/null 2>&1 || true
  sh scripts/vm-netns-ipsec.sh hub stop >/dev/null 2>&1 || true
  sh scripts/vm-vpp-ns-topology.sh clean >/dev/null 2>&1 || true
}

run_preflight() {
  run 'runtime preflight' sh scripts/vm-check.sh
  run 'VPP preflight' sh scripts/vm-vpp-preflight.sh
  run 'runtime status' sh scripts/vm-runtime-status.sh
}

run_underlay() {
  ensure_underlay
  run 'namespace underlay smoke' sh scripts/vm-netns.sh smoke
}

run_vpp() {
  vpp_smoke_mode=${VPP_SMOKE_MODE:-hub}
  ensure_underlay
  run 'VPP stale runtime cleanup' sh scripts/vm-vpp-ns-topology.sh clean
  run 'VPP namespace topology setup' env VPP_TOPOLOGY_MODE="$vpp_smoke_mode" SKIP_GRE=1 SKIP_IPSEC_TAP=1 \
    sh scripts/vm-vpp-ns-topology.sh setup
  trap cleanup_runtime EXIT INT TERM
  run 'VPP namespace forwarding smoke' sh scripts/vm-vpp-netns-smoke.sh
  run 'VPP namespace status' sh scripts/vm-vpp-ns-topology.sh status
  trap - EXIT INT TERM
  cleanup_runtime
}

run_ipsec() {
  ensure_underlay
  trap cleanup_runtime EXIT INT TERM
  run 'direct IPsec start' sh scripts/vm-netns-ipsec.sh direct start
  run 'direct encrypted traffic smoke' sh scripts/vm-netns-ipsec.sh direct smoke
  run 'direct IPsec status' sh scripts/vm-netns-ipsec.sh direct status
  trap - EXIT INT TERM
  cleanup_runtime
}

run_gre() {
  run 'GRE over IPsec controller smoke' sh scripts/vm-gre-namespace-v2-smoke.sh "$YAML"
}

run_integrated() {
  ensure_underlay
  trap cleanup_runtime EXIT INT TERM
  run 'direct IPsec start' sh scripts/vm-netns-ipsec.sh direct start
  run 'controller integrated direct/fallback smoke' env MODE=both \
    sh scripts/vm-controller-integrated-runtime-smoke.sh "$YAML"
  trap - EXIT INT TERM
  cleanup_runtime
}

case "$ACTION" in
  preflight)
    run_preflight
    ;;
  clean)
    require_root
    cleanup_runtime
    sh scripts/vm-netns.sh clean
    ;;
  underlay)
    require_root
    run_underlay
    ;;
  vpp)
    require_root
    run_vpp
    ;;
  ipsec)
    require_root
    run_ipsec
    ;;
  gre)
    require_root
    run_gre
    ;;
  integrated)
    require_root
    run_integrated
    ;;
  all)
    require_root
    run_preflight
    run_underlay
    run_vpp
    run_ipsec
    run_gre
    ;;
esac

printf '\nReal environment smoke passed: action=%s yaml=%s\n' "$ACTION" "$YAML"
