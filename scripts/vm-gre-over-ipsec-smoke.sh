#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
YAML=${1:-samples/gre-over-ipsec.yaml}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/gre-runtime}

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s [yaml]\n' "$0" >&2
  exit 1
fi
cd "$ROOT_DIR"

cleanup() {
  sh scripts/vm-netns-ipsec.sh gre stop >/dev/null 2>&1 || true
  sh scripts/vm-vpp-netns-clean.sh >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if ! ip netns exec site-a true >/dev/null 2>&1 || ! ip netns exec site-b true >/dev/null 2>&1; then
  sh scripts/vm-netns-setup.sh
fi
if [ ! -x "$BUILD_DIR/eventnet_netns_plan" ]; then
  BUILD_DIR="$BUILD_DIR" sh scripts/vm-build-cc.sh
fi
sh scripts/vm-netns-ipsec.sh gre stop >/dev/null 2>&1 || true
sh scripts/vm-vpp-netns-clean.sh >/dev/null 2>&1 || true
BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh "$YAML" --intent intent-gre-a-b >/dev/null
OUT_DIR="$OUT_DIR" sh "$OUT_DIR/apply-integrated.sh"

printf '%s\n' '== GRE over IPsec LAN ping =='
ip netns exec site-a ping -c 3 -W 2 10.10.2.1
printf '%s\n' '== GRE interface =='
vppctl show interface gre0
printf '%s\n' '== IPsec state =='
ip netns exec site-a ip xfrm state
printf '%s\n' 'GRE over IPsec smoke passed: SA, VPP GRE, and LAN forwarding succeeded.'
