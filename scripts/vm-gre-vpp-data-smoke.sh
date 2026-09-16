#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
VPPCTL=${VPPCTL:-vppctl}
YAML=${1:-samples/gre-vpp-data-plane.yaml}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/gre-vpp-data-plane}

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s [yaml]\n' "$0" >&2
  exit 1
fi

cd "$ROOT_DIR"
command -v ip >/dev/null 2>&1 || { printf 'missing: ip\n' >&2; exit 1; }
command -v "$VPPCTL" >/dev/null 2>&1 || { printf 'missing: %s\n' "$VPPCTL" >&2; exit 1; }

cleanup() {
  "$VPPCTL" create gre tunnel src 172.16.1.1 dst 172.16.2.2 instance 0 del >/dev/null 2>&1 || true
  "$VPPCTL" create gre tunnel src 172.16.2.1 dst 172.16.1.2 instance 1 del >/dev/null 2>&1 || true
  sh scripts/vm-vpp-netns-clean.sh >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if ! ip netns exec site-a true >/dev/null 2>&1 || ! ip netns exec site-b true >/dev/null 2>&1; then
  sh scripts/vm-netns-setup.sh
fi

VPP_TOPOLOGY=edge sh scripts/vm-vpp-netns-setup.sh

printf 'GRE VPP data path setup: %s\n' "$YAML"
if [ ! -x "$BUILD_DIR/eventnet_netns_plan" ]; then
  BUILD_DIR="$BUILD_DIR" sh scripts/vm-build-cc.sh
fi
BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --intent intent-gre-vpp-data
DRY_RUN=0 VPPCTL="$VPPCTL" sh "$OUT_DIR/vpp-netns-route-plan.sh"

"$VPPCTL" create gre tunnel src 172.16.2.1 dst 172.16.1.2 instance 1 del >/dev/null 2>&1 || true
"$VPPCTL" create gre tunnel src 172.16.2.1 dst 172.16.1.2 instance 1
"$VPPCTL" set interface ip address gre0 10.255.0.1/30
"$VPPCTL" set interface ip address gre1 10.255.0.2/30
"$VPPCTL" set interface state gre0 up
"$VPPCTL" set interface state gre1 up
"$VPPCTL" ip route add 10.10.2.0/24 via 10.255.0.2 gre0
"$VPPCTL" ip route add 10.10.1.0/24 via 10.255.0.1 gre1

printf '== GRE VPP data path: site-a -> site-b ==\n'
ip netns exec site-a ping -c 3 -W 2 -I 10.10.1.1 10.10.2.1
printf '== GRE VPP data path: site-b -> site-a ==\n'
ip netns exec site-b ping -c 3 -W 2 -I 10.10.2.1 10.10.1.1
printf '== GRE interfaces ==\n'
"$VPPCTL" show interface gre0
"$VPPCTL" show interface gre1
printf 'GRE VPP data smoke passed: bidirectional LAN traffic crossed VPP GRE.\n'
