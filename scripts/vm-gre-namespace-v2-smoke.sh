#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
YAML=${1:-samples/gre-namespace-v2.yaml}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/gre-namespace-v2}
RUN_BASE=${GRE_RUN_BASE:-/run/eventnet-netns-ipsec-gre-v2}
INTENT_ID=${INTENT_ID:-}
VPP_NATIVE_IPSEC=${VPP_NATIVE_IPSEC:-}

if [ "$(id -u)" != "0" ]; then printf 'Please run as root: sudo %s [yaml]\n' "$0" >&2; exit 1; fi
cd "$ROOT_DIR"
cleanup() {
  GRE_RUN_BASE="$RUN_BASE" sh scripts/vm-netns-ipsec-gre-stop.sh >/dev/null 2>&1 || true
  sh scripts/vm-vpp-ns-topology.sh clean >/dev/null 2>&1 || true
}
diagnose_datapath() {
  for ns in site-a site-b; do
    printf '== datapath diagnostics: %s ==\n' "$ns" >&2
    ip netns exec "$ns" ip -br addr >&2 || true
    ip netns exec "$ns" ip route >&2 || true
    ip netns exec "$ns" ip neigh >&2 || true
    ip netns exec "$ns" ip -br link >&2 || true
    ip netns exec "$ns" ip xfrm state >&2 || true
    ip netns exec "$ns" ip -s xfrm policy >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show interface >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show hardware-interfaces >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show ip fib >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show trace >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show error >&2 || true
  done
}
trap cleanup EXIT INT TERM

if ! ip netns exec site-a true >/dev/null 2>&1 || ! ip netns exec site-b true >/dev/null 2>&1; then sh scripts/vm-netns-setup.sh; fi
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  printf 'Building the controller before generating the namespace plan...\n'
  FORCE_REBUILD="${FORCE_REBUILD:-1}" BUILD_DIR="$BUILD_DIR" sh scripts/vm-build-cc.sh
fi
if [ -z "$INTENT_ID" ]; then
  INTENT_ID=$("$BUILD_DIR/eventnet_yaml_demo" "$YAML" 2>/dev/null | awk '/^intent:/{print $2; exit}')
  if [ -z "$INTENT_ID" ]; then
    printf 'Could not determine an intent from %s. Set INTENT_ID explicitly.\n' "$YAML" >&2
    exit 1
  fi
  printf 'Auto-selected intent: %s\n' "$INTENT_ID"
fi
if [ -z "$VPP_NATIVE_IPSEC" ]; then
  if grep -q '^    vpp_local_sa_id:' "$YAML" && grep -q '^    vpp_crypto_algorithm:' "$YAML"; then
    VPP_NATIVE_IPSEC=1
  else
    VPP_NATIVE_IPSEC=0
  fi
fi
if [ "$VPP_NATIVE_IPSEC" = "1" ]; then
  printf 'Auto-selected VPP Native IPsec mode.\n'
fi
# The generated plan owns GRE. Creating it here too leaves stale FIB paths
# when the plan deletes and recreates gre0.
SKIP_GRE=1 sh scripts/vm-vpp-ns-topology.sh setup
BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --intent "$INTENT_ID"
if [ "$VPP_NATIVE_IPSEC" = "1" ]; then
  printf 'Using VPP Native IPsec; strongSwan is not started for this smoke.\n'
else
  GRE_CHILD=gre-namespace-v2 GRE_OUTER_LOCAL_ENDPOINT=198.18.1.1 GRE_OUTER_REMOTE_ENDPOINT=198.18.2.1 GRE_RUN_BASE="$RUN_BASE" sh scripts/vm-netns-ipsec-gre-start.sh "$OUT_DIR"
fi
DRY_RUN=0 sh "$OUT_DIR/vpp-netns-route-plan.sh"
if [ "$VPP_NATIVE_IPSEC" = "1" ]; then
  for ns in site-a site-b; do
    printf '== VPP Native IPsec status: %s ==\n' "$ns"
    vpp_socket="/run/ibuki-vpp-ns/$ns/cli.sock"
    vppctl -s "$vpp_socket" show ipsec all 2>/dev/null || true
    vppctl -s "$vpp_socket" show ipsec protect 2>/dev/null || true
    vppctl -s "$vpp_socket" show interface 2>/dev/null || true
    vppctl -s "$vpp_socket" show ip fib 2>/dev/null || true
  done
fi
printf '== namespaced GRE over IPsec: site-a -> site-b ==\n'
vppctl -s /run/ibuki-vpp-ns/site-a/cli.sock trace add af-packet-input 20 >/dev/null 2>&1 || true
vppctl -s /run/ibuki-vpp-ns/site-b/cli.sock trace add af-packet-input 20 >/dev/null 2>&1 || true
if ! ip netns exec site-a ping -c 3 -W 2 -I 10.10.1.1 10.10.2.1; then
  diagnose_datapath
  exit 1
fi
printf '== namespaced GRE over IPsec: site-b -> site-a ==\n'
if ! ip netns exec site-b ping -c 3 -W 2 -I 10.10.2.1 10.10.1.1; then
  diagnose_datapath
  exit 1
fi
if [ "$VPP_NATIVE_IPSEC" = "1" ]; then
  for ns in site-a site-b; do
    printf '== VPP Native IPsec traffic counters: %s ==\n' "$ns"
    vpp_socket="/run/ibuki-vpp-ns/$ns/cli.sock"
    vppctl -s "$vpp_socket" show ipsec sa
    vpp_errors=$(vppctl -s "$vpp_socket" show errors)
    printf '%s\n' "$vpp_errors"
    for node in esp4-encrypt-tun esp4-decrypt-tun; do
      if ! printf '%s\n' "$vpp_errors" | awk -v node="$node" '$2 == node && $1 + 0 > 0 { found=1 } END { exit !found }'; then
        printf 'No %s traffic observed in %s.\n' "$node" "$ns" >&2
        diagnose_datapath
        exit 1
      fi
    done
  done
fi
if [ "$VPP_NATIVE_IPSEC" = "0" ]; then
  printf '== strongSwan/XFRM ==\n'
  xfrm_state=$(ip netns exec site-a ip xfrm state)
  printf '%s\n' "$xfrm_state"
  if ! printf '%s\n' "$xfrm_state" | grep -Eq 'anti-replay context: seq 0x[1-9a-f]' ||
     ! printf '%s\n' "$xfrm_state" | grep -Eq 'oseq 0x[1-9a-f]'; then
    printf 'ESP receive/send sequence did not advance.\n' >&2
    diagnose_datapath
    exit 1
  fi
fi
printf 'Namespaced GRE over IPsec smoke passed.\n'
