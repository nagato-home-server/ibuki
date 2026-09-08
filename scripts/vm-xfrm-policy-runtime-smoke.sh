#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_PREFIX=${SOURCE_PREFIX:-10.10.1.0/24}
DESTINATION_PREFIX=${DESTINATION_PREFIX:-10.10.2.0/24}
BLOCK_PRIORITY=${BLOCK_PRIORITY:-10000}

if [ "$(id -u)" != "0" ]; then
  printf '%s\n' 'Please run as root: sudo sh scripts/vm-xfrm-policy-runtime-smoke.sh' >&2
  exit 1
fi
case "$BLOCK_PRIORITY" in
  ''|*[!0-9]*) printf 'invalid BLOCK_PRIORITY: %s\n' "$BLOCK_PRIORITY" >&2; exit 2 ;;
esac
cd "$ROOT_DIR"
mkdir -p "$ROOT_DIR/out"

if ! ip netns list | awk '{print $1}' | grep -qx site-a ||
   ! ip netns list | awk '{print $1}' | grep -qx site-b; then
  sh scripts/vm-netns-setup.sh
fi

for ns in site-a site-b; do
  ip netns list | awk '{print $1}' | grep -qx "$ns" || {
    printf 'missing namespace: %s. Run sh scripts/vm-netns-setup.sh first.\n' "$ns" >&2
    exit 1
  }
done

cleanup() {
  ip netns exec site-a ip xfrm policy delete dir out src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" 2>/dev/null || true
  ip netns exec site-a ip xfrm policy delete dir in src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" 2>/dev/null || true
  ip netns exec site-b ip xfrm policy delete dir out src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" 2>/dev/null || true
  ip netns exec site-b ip xfrm policy delete dir in src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
cleanup

ip netns exec site-a ip xfrm policy add dir out src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" action block
ip netns exec site-a ip xfrm policy add dir in src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" action block
ip netns exec site-b ip xfrm policy add dir out src "$DESTINATION_PREFIX" dst "$SOURCE_PREFIX" priority "$BLOCK_PRIORITY" action block
ip netns exec site-b ip xfrm policy add dir in src "$SOURCE_PREFIX" dst "$DESTINATION_PREFIX" priority "$BLOCK_PRIORITY" action block

ip netns exec site-a ip xfrm policy list > "$ROOT_DIR/out/xfrm-policy-runtime-site-a.txt"
ip netns exec site-b ip xfrm policy list > "$ROOT_DIR/out/xfrm-policy-runtime-site-b.txt"

assert_policy() {
  policy_file=$1
  expected_dir=$2
  expected_src=$3
  expected_dst=$4
  awk -v expected_dir="$expected_dir" -v expected_src="$expected_src" -v expected_dst="$expected_dst" '
    /^[[:space:]]*src / { source = $2; destination = $4 }
    /^[[:space:]]*dir / {
      if (source == expected_src && destination == expected_dst && index($0, "dir " expected_dir) > 0) found = 1
    }
    END { exit(found ? 0 : 1) }
  ' "$policy_file"
}

assert_policy "$ROOT_DIR/out/xfrm-policy-runtime-site-a.txt" out "$SOURCE_PREFIX" "$DESTINATION_PREFIX"
assert_policy "$ROOT_DIR/out/xfrm-policy-runtime-site-a.txt" in "$DESTINATION_PREFIX" "$SOURCE_PREFIX"
assert_policy "$ROOT_DIR/out/xfrm-policy-runtime-site-b.txt" out "$DESTINATION_PREFIX" "$SOURCE_PREFIX"
assert_policy "$ROOT_DIR/out/xfrm-policy-runtime-site-b.txt" in "$SOURCE_PREFIX" "$DESTINATION_PREFIX"
grep -q "priority $BLOCK_PRIORITY" "$ROOT_DIR/out/xfrm-policy-runtime-site-a.txt"
grep -q "priority $BLOCK_PRIORITY" "$ROOT_DIR/out/xfrm-policy-runtime-site-b.txt"

printf '%s\n' 'XFRM runtime policy smoke passed: scoped bidirectional block policies installed.'
