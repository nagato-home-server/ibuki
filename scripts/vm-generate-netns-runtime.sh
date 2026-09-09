#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-runtime}"
YAML="${1:-samples/linux-vm-netns.yaml}"
shift 2>/dev/null || true

if [ ! -x "$BUILD_DIR/eventnet_netns_plan" ]; then
  printf 'eventnet_netns_plan not found. Run sh scripts/vm-build-cc.sh first.\n' >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
cd "$ROOT_DIR"

set +e
"$BUILD_DIR/eventnet_netns_plan" --out-dir "$OUT_DIR" "$@" "$YAML"
plan_status=$?
set -e
if [ "$plan_status" -ne 0 ] && [ "${ALLOW_UNSUPPORTED:-0}" != "1" ]; then
  exit "$plan_status"
fi
if [ "$plan_status" -ne 0 ]; then
  printf 'runtime warning: eventnet_netns_plan returned status %s; generated plans are available for inspection only.\n' "$plan_status" >&2
fi
chmod +x "$OUT_DIR/apply-selected.sh"
chmod +x "$OUT_DIR/apply-integrated.sh"
chmod +x "$OUT_DIR/rollback-selected.sh"
chmod +x "$OUT_DIR/vpp-route-plan.sh"
chmod +x "$OUT_DIR/vpp-netns-route-plan.sh"

printf '\nGenerated netns runtime files:\n'
printf '  %s/apply-selected.sh\n' "$OUT_DIR"
printf '  %s/apply-integrated.sh\n' "$OUT_DIR"
printf '  %s/rollback-selected.sh\n' "$OUT_DIR"
printf '  %s/selected-path.txt\n' "$OUT_DIR"
printf '  %s/vpp-route-plan.sh\n' "$OUT_DIR"
printf '  %s/vpp-netns-route-plan.sh\n' "$OUT_DIR"
