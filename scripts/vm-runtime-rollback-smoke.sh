#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/netns-runtime-rollback}
LOG_FILE=$OUT_DIR/injected-failure.log

if [ "$(id -u)" != "0" ]; then
  printf '%s\n' 'Please run as root: sudo sh scripts/vm-runtime-rollback-smoke.sh' >&2
  exit 1
fi
cd "$ROOT_DIR"

if ! ip netns exec site-a true >/dev/null 2>&1 || ! ip netns exec site-b true >/dev/null 2>&1; then
  sh scripts/vm-netns-setup.sh
fi
BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml --path path-direct >/dev/null
mkdir -p "$OUT_DIR"

cleanup() {
  sh "$OUT_DIR/rollback-selected.sh" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

set +e
EVENTNET_INJECT_FAILURE=after-runtime OUT_DIR="$OUT_DIR" sh "$OUT_DIR/apply-selected.sh" >"$LOG_FILE" 2>&1
status=$?
set -e
if [ "$status" -ne 97 ]; then
  printf 'expected injected status 97, got %s\n' "$status" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi
grep -q 'injected runtime failure for rollback smoke' "$LOG_FILE"

for ns in site-a site-b; do
  if [ -e "/run/eventnet-netns-ipsec-direct/$ns/charon.pid" ]; then
    printf 'charon pid file remains after rollback: %s\n' "$ns" >&2
    exit 1
  fi
done
if ip netns exec site-a ip xfrm state | grep -q '^src '; then
  printf '%s\n' 'XFRM state remains after rollback' >&2
  exit 1
fi

printf '%s\n' 'Runtime rollback smoke passed: injected apply failure cleaned direct IPsec state.'
