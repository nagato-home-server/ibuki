#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-linux-cc}
OUT_DIR=${OUT_DIR:-$ROOT_DIR/out/xfrm-policy-smoke}
YAML=${1:-samples/route-examples.yaml}

cd "$ROOT_DIR"
mkdir -p "$OUT_DIR"
if [ ! -x "$BUILD_DIR/eventnet_yaml_demo" ]; then
  printf '%s\n' 'eventnet_yaml_demo not found. Run sh scripts/vm-build-cc.sh first.' >&2
  exit 1
fi

"$BUILD_DIR/eventnet_yaml_demo" --intent intent-vlan-direct \
  --conf "$OUT_DIR/eventnet-swanctl.conf" --emit-script "$OUT_DIR/apply.sh" "$YAML" > "$OUT_DIR/blocked.txt"
grep -q 'ip xfrm policy add dir out src ' "$OUT_DIR/blocked.txt"
grep -q 'ip xfrm policy add dir in src ' "$OUT_DIR/blocked.txt"
grep -q 'ip xfrm policy delete dir out src ' "$OUT_DIR/blocked.txt"
grep -q 'ip xfrm policy delete dir in src ' "$OUT_DIR/blocked.txt"
[ "$(grep -c 'ip xfrm policy add dir ' "$OUT_DIR/blocked.txt")" -eq 2 ]
[ "$(grep -c 'ip xfrm policy delete dir ' "$OUT_DIR/blocked.txt")" -eq 2 ]

"$BUILD_DIR/eventnet_yaml_demo" --intent intent-a-b \
  --conf "$OUT_DIR/default-swanctl.conf" "$ROOT_DIR/samples/linux-vm-netns.yaml" > "$OUT_DIR/default.txt"
if grep -q 'ip xfrm policy add dir ' "$OUT_DIR/default.txt"; then
  printf '%s\n' 'default Intent unexpectedly generated XFRM block policy' >&2
  exit 1
fi

printf '%s\n' 'XFRM policy smoke passed: scoped bidirectional block and default-off behavior verified.'
