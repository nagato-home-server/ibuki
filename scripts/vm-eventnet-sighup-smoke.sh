#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ibuki-sighup.XXXXXX")
PID=""

cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnetd" ] || [ ! -x "$BUILD_DIR/eventnet_agent" ]; then
  sh scripts/vm-build-cc.sh
fi

cp samples/linux-vm-netns.yaml "$TMP_DIR/config.yaml"
"$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 \
  --count 1 --simulate 12 0 --output "$TMP_DIR/telemetry.jsonl" >/dev/null

"$BUILD_DIR/eventnetd" "$TMP_DIR/config.yaml" --telemetry "$TMP_DIR/telemetry.jsonl" \
  --reload-config --reload-on-sighup --state-file "$TMP_DIR/state.tsv" --count 2 \
  --max-age-ms 0 >"$TMP_DIR/stdout.log" 2>"$TMP_DIR/stderr.log" &
PID=$!

i=0
while ! grep -q 'sighup_reload: waiting' "$TMP_DIR/stdout.log" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -ge 50 ]; then
    cat "$TMP_DIR/stdout.log" "$TMP_DIR/stderr.log" >&2 || true
    printf '%s\n' 'eventnetd did not enter SIGHUP wait state' >&2
    exit 1
  fi
  sleep 0.1
done

kill -HUP "$PID"
sleep 0.3
printf '%s\n' 'invalid: [' > "$TMP_DIR/config.yaml"
kill -HUP "$PID"
wait "$PID"
PID=""

grep -q 'config_reload: applied' "$TMP_DIR/stdout.log"
grep -q 'keeping previous configuration' "$TMP_DIR/stderr.log"
grep -q 'selected_path: path-direct' "$TMP_DIR/stdout.log"

cat "$TMP_DIR/stdout.log"
cat "$TMP_DIR/stderr.log"
printf '%s\n' 'EventNet SIGHUP smoke passed: valid reload and invalid-config retention succeeded.'
