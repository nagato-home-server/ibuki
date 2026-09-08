#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
YAML="${1:-$ROOT_DIR/samples/linux-vm-netns.yaml}"
TELEMETRY="${TELEMETRY:-$ROOT_DIR/samples/telemetry-replay.jsonl}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ibuki-status.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnetd" ]; then
  sh scripts/vm-build-cc.sh
fi

normal="$TMP_DIR/status.jsonl"
"$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$TELEMETRY" \
  --batch-size 3 --count 1 --max-age-ms 0 --status-jsonl "$normal" > "$TMP_DIR/normal.log"
test -s "$normal"
grep -q 'selected_path: path-direct' "$TMP_DIR/normal.log"

target="$TMP_DIR/status-target.jsonl"
: > "$target"
symlink="$TMP_DIR/status-symlink.jsonl"
ln -s "$target" "$symlink"
if "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$TELEMETRY" \
  --batch-size 3 --count 1 --max-age-ms 0 --status-jsonl "$symlink" > "$TMP_DIR/symlink.log" 2>&1; then
  printf '%s\n' 'status symlink unexpectedly accepted' >&2
  exit 1
fi
grep -q 'status output open failed' "$TMP_DIR/symlink.log"

unsafe="$TMP_DIR/status-unsafe.jsonl"
: > "$unsafe"
chmod 0660 "$unsafe"
if "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$TELEMETRY" \
  --batch-size 3 --count 1 --max-age-ms 0 --status-jsonl "$unsafe" > "$TMP_DIR/unsafe.log" 2>&1; then
  printf '%s\n' 'group-writable status unexpectedly accepted' >&2
  exit 1
fi
grep -q 'status output open failed' "$TMP_DIR/unsafe.log"

printf '%s\n' 'eventnetd status security smoke passed: regular output works and symlink/group-writable targets are rejected.'
