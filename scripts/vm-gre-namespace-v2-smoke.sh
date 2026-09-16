#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
YAML=${1:-samples/gre-namespace-v2.yaml}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/gre-namespace-v2}
RUN_BASE=${GRE_RUN_BASE:-/run/eventnet-netns-ipsec-gre-v2}

if [ "$(id -u)" != "0" ]; then printf 'Please run as root: sudo %s [yaml]\n' "$0" >&2; exit 1; fi
cd "$ROOT_DIR"
cleanup() {
  OUT_DIR="$OUT_DIR" RUN_BASE="$RUN_BASE" sh scripts/vm-netns-ipsec-gre-stop.sh >/dev/null 2>&1 || true
  sh scripts/vm-vpp-ns-topology.sh clean >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if ! ip netns exec site-a true >/dev/null 2>&1 || ! ip netns exec site-b true >/dev/null 2>&1; then sh scripts/vm-netns-setup.sh; fi
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  printf 'Building the controller before generating the namespace plan...\n'
  FORCE_REBUILD="${FORCE_REBUILD:-1}" BUILD_DIR="$BUILD_DIR" sh scripts/vm-build-cc.sh
fi
sh scripts/vm-vpp-ns-topology.sh setup
BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --intent intent-gre-namespace-v2
GRE_CHILD=gre-namespace-v2 GRE_DYNAMIC_TS=1 GRE_OUTER_LOCAL_ENDPOINT=198.18.1.1 GRE_OUTER_REMOTE_ENDPOINT=198.18.2.1 RUN_BASE="$RUN_BASE" sh scripts/vm-netns-ipsec-gre-start.sh "$OUT_DIR"
DRY_RUN=0 VPPCTL_SOCKET=/run/ibuki-vpp-ns/site-a/cli.sock sh "$OUT_DIR/vpp-netns-route-plan.sh"
printf '== namespaced GRE over IPsec: site-a -> site-b ==\n'
ip netns exec site-a ping -c 3 -W 2 -I 10.10.1.1 10.10.2.1
printf '== namespaced GRE over IPsec: site-b -> site-a ==\n'
ip netns exec site-b ping -c 3 -W 2 -I 10.10.2.1 10.10.1.1
printf '== strongSwan/XFRM ==\n'
ip netns exec site-a ip xfrm state
printf 'Namespaced GRE over IPsec smoke passed.\n'
