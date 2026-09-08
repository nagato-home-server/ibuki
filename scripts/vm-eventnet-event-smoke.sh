#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/eventnet-event-smoke}"

cd "$ROOT_DIR"
if [ ! -x "$BUILD_DIR/eventnetd" ] || [ ! -x "$BUILD_DIR/eventnet_swanctl_observer" ] || [ ! -x "$BUILD_DIR/eventnet_vpp_observer" ] || [ ! -x "$BUILD_DIR/eventnet_vpp_interface_observer" ]; then sh scripts/vm-build-cc.sh; fi
mkdir -p "$OUT_DIR"

timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.event.path.v1","event":"path_failed","path_id":"path-direct","timestamp_ms":%s}\n' "$timestamp_ms" > "$OUT_DIR/failure.jsonl"
printf '{"schema":"ibuki.event.path.v1","event":"path_recovered","path_id":"path-via-hub","timestamp_ms":%s}\n' "$timestamp_ms" >> "$OUT_DIR/failure.jsonl"
printf '{"schema":"ibuki.event.path.v1","event":"path_recovered","path_id":"path-direct","timestamp_ms":%s}\n' "$timestamp_ms" > "$OUT_DIR/recovery.jsonl"
printf '{"schema":"ibuki.event.path.v1","event":"path_failed","path_id":"path-via-hub","timestamp_ms":%s}\n' "$timestamp_ms" >> "$OUT_DIR/recovery.jsonl"

"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/failure.jsonl" > "$OUT_DIR/failure.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/failure.log"

"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/recovery.jsonl" > "$OUT_DIR/recovery.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/recovery.log"

cat samples/swanctl-list-sas-installed.txt | "$BUILD_DIR/eventnet_swanctl_observer" - tun-a-b --event path-direct > "$OUT_DIR/swanctl-event.jsonl"
cat "$OUT_DIR/swanctl-event.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry-stdin --count 1 > "$OUT_DIR/swanctl-event.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/swanctl-event.log"
sed 's/"tunnel_id":"tun-a-b"/"tunnel_id":"tun-unknown"/' "$OUT_DIR/swanctl-event.jsonl" > "$OUT_DIR/swanctl-event-mismatch.jsonl"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry "$OUT_DIR/swanctl-event-mismatch.jsonl" --once > "$OUT_DIR/swanctl-mismatch.log" 2>&1; then
  printf '%s\n' 'mismatched strongSwan tunnel unexpectedly accepted' >&2
  exit 1
fi
grep -q 'telemetry identity rejected: telemetry tunnel does not belong to path' "$OUT_DIR/swanctl-mismatch.log"

cat samples/vpp-show-ip-fib.txt | "$BUILD_DIR/eventnet_vpp_observer" - 10.10.2.0/24 --event path-direct route-a-b > "$OUT_DIR/vpp-event.jsonl"
cat "$OUT_DIR/vpp-event.jsonl" | "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b --telemetry-stdin --count 1 > "$OUT_DIR/vpp-event.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/vpp-event.log"

"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/recovery.jsonl" --backend command \
  --swanctl-uri unix:///run/eventnet/site-a/charon.vici \
  --swanctl-config "$OUT_DIR/eventnet-swanctl.conf" --once > "$OUT_DIR/swanctl-config.log"
grep -q 'load-conns --file' "$OUT_DIR/swanctl-config.log"
grep -q 'initiate --child tun-a-b' "$OUT_DIR/swanctl-config.log"
load_line=$(grep -n 'load-conns --file' "$OUT_DIR/swanctl-config.log" | cut -d: -f1)
initiate_line=$(grep -n 'initiate --child tun-a-b' "$OUT_DIR/swanctl-config.log" | cut -d: -f1)
[ "$load_line" -lt "$initiate_line" ]

attr_timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-route-attributes","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":%s}\n' "$attr_timestamp_ms" > "$OUT_DIR/route-attributes.jsonl"
"$BUILD_DIR/eventnetd" samples/route-examples.yaml --intent intent-route-attributes \
  --telemetry "$OUT_DIR/route-attributes.jsonl" --backend command \
  --swanctl-uri unix:///run/eventnet/site-a/charon.vici \
  --swanctl-config "$OUT_DIR/eventnet-swanctl.conf" \
  --vppctl-socket /run/vpp/cli.sock --once > "$OUT_DIR/route-attributes.log"
table_line=$(grep -n 'ip table add 100' "$OUT_DIR/route-attributes.log" | cut -d: -f1)
route_line=$(grep -n 'ip route add 10.10.2.0/24 table 100 preference 20 via 203.0.113.9 ipsec0' "$OUT_DIR/route-attributes.log" | cut -d: -f1)
[ -n "$table_line" ]
[ -n "$route_line" ]
[ "$table_line" -lt "$route_line" ]

vlan_timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-vpp-vlan","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":%s}\n' "$vlan_timestamp_ms" > "$OUT_DIR/vlan-command.jsonl"
printf '{"schema":"ibuki.event.vpp.route.v1","path_id":"path-vpp-vlan","tunnel_id":"tun-vlan-a-b","destination_prefix":"10.10.2.0/24","next_hop":"172.16.101.2","state":"up","timestamp_ms":%s}\n' "$vlan_timestamp_ms" > "$OUT_DIR/vlan-route.jsonl"
cat "$OUT_DIR/vlan-route.jsonl" "$OUT_DIR/vlan-command.jsonl" > "$OUT_DIR/vlan-command-combined.jsonl"
"$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-command-combined.jsonl" --backend command \
  --vppctl-socket /run/vpp/cli.sock --batch-size 2 --once > "$OUT_DIR/vlan-command.log"
grep -q 'create sub-interfaces host-vpp-site-a 100' "$OUT_DIR/vlan-command.log"
grep -q 'set interface host-vpp-site-a.100 up' "$OUT_DIR/vlan-command.log"
grep -q 'create sub-interfaces host-vpp-site-b 100' "$OUT_DIR/vlan-command.log"
grep -q 'ip route add 10.10.2.0/24 table 0 via 172.16.101.2' "$OUT_DIR/vlan-command.log"
grep -q 'set acl-plugin acl index 10100' "$OUT_DIR/vlan-command.log"
grep -q 'set acl-plugin interface host-vpp-site-a input acl 10100' "$OUT_DIR/vlan-command.log"
grep -q 'set acl-plugin interface host-vpp-site-b input acl 10101' "$OUT_DIR/vlan-command.log"

cat samples/vpp-show-interface.txt | "$BUILD_DIR/eventnet_vpp_interface_observer" - host-vpp-site-a.100 --event path-vpp-vlan > "$OUT_DIR/vlan-interface-observer.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-observer.jsonl" --once > "$OUT_DIR/vlan-interface-observer.log" 2>&1; then
  printf '%s\n' 'interface-only VLAN observation unexpectedly kept the path usable' >&2
  exit 1
fi
grep -q 'intent failed: no_candidate' "$OUT_DIR/vlan-interface-observer.log"
cat "$OUT_DIR/vlan-route.jsonl" "$OUT_DIR/vlan-interface-observer.jsonl" > "$OUT_DIR/vlan-observer-combined.jsonl"
"$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-observer-combined.jsonl" --batch-size 2 --once > "$OUT_DIR/vlan-observer-combined.log"
grep -q 'selected_path: path-vpp-vlan' "$OUT_DIR/vlan-observer-combined.log"
cat "$OUT_DIR/vlan-route.jsonl" "$OUT_DIR/vlan-interface-observer.jsonl" | \
  "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry-stdin --count 2 --max-age-ms 0 > "$OUT_DIR/vlan-observer-stream.log" 2>&1
grep -q 'intent unavailable: no_candidate; waiting for next telemetry batch' "$OUT_DIR/vlan-observer-stream.log"
grep -q 'selected_path: path-vpp-vlan' "$OUT_DIR/vlan-observer-stream.log"
printf '%s\n' 'host-vpp-site-b.100 up' | "$BUILD_DIR/eventnet_vpp_interface_observer" - host-vpp-site-a.100 --event path-vpp-vlan > "$OUT_DIR/vlan-interface-missing.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-missing.jsonl" --once > "$OUT_DIR/vlan-interface-missing.log" 2>&1; then
  printf '%s\n' 'missing VLAN interface unexpectedly kept the path usable' >&2
  exit 1
fi
grep -q 'intent failed: no_candidate' "$OUT_DIR/vlan-interface-missing.log"

vlan_interface_timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.event.vpp.interface.v1","path_id":"path-vpp-vlan","interface_name":"host-vpp-site-a.100","state":"down","timestamp_ms":%s}\n' "$vlan_interface_timestamp_ms" > "$OUT_DIR/vlan-interface-down.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-down.jsonl" --once > "$OUT_DIR/vlan-interface-down.log" 2>&1; then
  printf '%s\n' 'down VLAN interface unexpectedly kept the path usable' >&2
  exit 1
fi
grep -q 'intent failed: no_candidate' "$OUT_DIR/vlan-interface-down.log"
printf '{"schema":"ibuki.event.vpp.interface.v1","path_id":"path-vpp-vlan","interface_name":"host-vpp-site-a.100","state":"up","timestamp_ms":%s}\n' "$vlan_interface_timestamp_ms" > "$OUT_DIR/vlan-interface-up.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-up.jsonl" --once > "$OUT_DIR/vlan-interface-up.log"
then
  printf '%s\n' 'interface-only VLAN up unexpectedly kept the path usable' >&2
  exit 1
fi
grep -q 'intent failed: no_candidate' "$OUT_DIR/vlan-interface-up.log"
sed 's/host-vpp-site-a\.100/host-vpp-site-a/' "$OUT_DIR/vlan-interface-up.jsonl" > "$OUT_DIR/vlan-interface-parent.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-parent.jsonl" --once > "$OUT_DIR/vlan-interface-parent.log" 2>&1; then
  printf '%s\n' 'parent interface unexpectedly satisfied VLAN policy' >&2
  exit 1
fi
grep -q 'telemetry identity rejected: telemetry interface does not belong to path' "$OUT_DIR/vlan-interface-parent.log"
sed 's/host-vpp-site-a\.100/host-vpp-unknown.200/' "$OUT_DIR/vlan-interface-up.jsonl" > "$OUT_DIR/vlan-interface-mismatch.jsonl"
if "$BUILD_DIR/eventnetd" samples/vpp-vlan-netns.yaml --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-mismatch.jsonl" --once > "$OUT_DIR/vlan-interface-mismatch.log" 2>&1; then
  printf '%s\n' 'interface from another VPP edge unexpectedly accepted' >&2
  exit 1
fi
grep -q 'telemetry identity rejected: telemetry interface does not belong to path' "$OUT_DIR/vlan-interface-mismatch.log"

sed 's/allowed_vlans: \[1, 100, 4094\]/allowed_vlans: [1, 200]/' samples/vpp-vlan-netns.yaml > "$OUT_DIR/vlan-restricted.yaml"
if "$BUILD_DIR/eventnetd" "$OUT_DIR/vlan-restricted.yaml" --intent intent-vpp-vlan \
  --telemetry "$OUT_DIR/vlan-interface-up.jsonl" --once > "$OUT_DIR/vlan-allowlist-event.log" 2>&1; then
  printf '%s\n' 'disallowed VLAN telemetry unexpectedly kept the path usable' >&2
  exit 1
fi
grep -q 'telemetry identity rejected: telemetry interface does not belong to path' "$OUT_DIR/vlan-allowlist-event.log"

xfrm_timestamp_ms=$(($(date +%s) * 1000))
printf '{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-legacy-single-route","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":%s}\n' "$xfrm_timestamp_ms" > "$OUT_DIR/xfrm-command.jsonl"
"$BUILD_DIR/eventnetd" samples/route-examples.yaml --intent intent-vlan-direct \
  --telemetry "$OUT_DIR/xfrm-command.jsonl" --backend command --once > "$OUT_DIR/xfrm-command.log"
grep -q 'ip xfrm policy add dir out src 10.10.1.0/24 dst 10.10.2.0/24 priority 10000 action block' "$OUT_DIR/xfrm-command.log"
grep -q 'ip xfrm policy add dir in src 10.10.2.0/24 dst 10.10.1.0/24 priority 10000 action block' "$OUT_DIR/xfrm-command.log"

FAKE_BIN="$OUT_DIR/fake-bin"
FAKE_LOG="$OUT_DIR/fake-command.log"
mkdir -p "$FAKE_BIN"
cat > "$FAKE_BIN/ip" <<'SH'
#!/usr/bin/env sh
if [ "$*" = "xfrm policy list" ]; then
  cat <<'POLICY'
src 10.10.1.0/24 dst 10.10.2.0/24
	dir out priority 10000
	action block

POLICY
  if [ "${XFRM_ASYMMETRIC:-0}" != "1" ]; then
    cat <<'POLICY'
src 10.10.2.0/24 dst 10.10.1.0/24
	dir in priority 10000
	action block
POLICY
  fi
  exit 0
fi
printf 'ip %s\n' "$*" >> "$FAKE_LOG"
exit 0
SH
cat > "$FAKE_BIN/swanctl" <<'SH'
#!/usr/bin/env sh
printf 'swanctl %s\n' "$*" >> "$FAKE_LOG"
exit 0
SH
cat > "$FAKE_BIN/vppctl" <<'SH'
#!/usr/bin/env sh
printf 'vppctl %s\n' "$*" >> "$FAKE_LOG"
exit 0
SH
chmod +x "$FAKE_BIN/ip" "$FAKE_BIN/swanctl" "$FAKE_BIN/vppctl"
FAKE_LOG="$FAKE_LOG" PATH="$FAKE_BIN:$PATH" "$BUILD_DIR/eventnetd" samples/route-examples.yaml --intent intent-vlan-direct \
  --telemetry "$OUT_DIR/xfrm-command.jsonl" --backend command --apply --once > "$OUT_DIR/xfrm-reuse.log"
grep -q 'swanctl --initiate --child tun-a-b' "$FAKE_LOG"
if grep -q 'ip xfrm policy add\|ip xfrm policy delete' "$FAKE_LOG"; then
  printf '%s\n' 'existing XFRM policies were unexpectedly changed' >&2
  exit 1
fi
if XFRM_ASYMMETRIC=1 FAKE_LOG="$FAKE_LOG" PATH="$FAKE_BIN:$PATH" "$BUILD_DIR/eventnetd" samples/route-examples.yaml --intent intent-vlan-direct \
  --telemetry "$OUT_DIR/xfrm-command.jsonl" --backend command --apply --once > "$OUT_DIR/xfrm-asymmetric.log" 2>&1; then
  printf '%s\n' 'asymmetric XFRM policy state was unexpectedly accepted' >&2
  exit 1
fi
grep -q 'intent failed: state_conflict' "$OUT_DIR/xfrm-asymmetric.log"

SOCKET_PATH="$OUT_DIR/eventnetd.sock"
STATE_FILE="$OUT_DIR/socket-state.tsv"
rm -f "$SOCKET_PATH"
rm -f "$STATE_FILE"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-socket "$SOCKET_PATH" --socket-accept-count 0 --socket-retry-count 1 --socket-retry-backoff-ms 10 --socket-uid "$(id -u)" \
  --state-file "$STATE_FILE" > "$OUT_DIR/socket.log" 2>&1 &
DAEMON_PID=$!
trap 'kill "$DAEMON_PID" 2>/dev/null || true; rm -f "$SOCKET_PATH"' EXIT
for attempt in $(seq 1 50); do
  [ -S "$SOCKET_PATH" ] && break
  sleep 0.1
done
[ -S "$SOCKET_PATH" ]
python3 - "$SOCKET_PATH" "$OUT_DIR/failure.jsonl" "$OUT_DIR/recovery.jsonl" <<'PY'
import socket
import sys

path, failure, recovery = sys.argv[1:]
for filename in (None, failure, recovery):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(path)
        if filename is not None:
            with open(filename, "rb") as source:
                client.sendall(source.read())
PY
kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
grep -q 'eventnetd_iteration: 1/1' "$OUT_DIR/socket.log"
grep -q 'socket_reconnect_retry: 1/1' "$OUT_DIR/socket.log"
grep -q 'state_restored: 1' "$OUT_DIR/socket.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/socket.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/socket.log"
[ ! -e "$SOCKET_PATH" ]
rm -f "$SOCKET_PATH"
trap - EXIT

SHARED_SOCKET_PATH="$OUT_DIR/eventnetd-shared.sock"
SHARED_STATE_FILE="$OUT_DIR/shared-state.tsv"
rm -f "$SHARED_SOCKET_PATH" "$SHARED_STATE_FILE"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-socket "$SHARED_SOCKET_PATH" --socket-accept-count 2 --socket-parallel --socket-parallel-timeout-ms 5000 \
  --state-file "$SHARED_STATE_FILE" > "$OUT_DIR/shared.log" 2>&1 &
SHARED_DAEMON_PID=$!
trap 'kill "$SHARED_DAEMON_PID" 2>/dev/null || true; rm -f "$SHARED_SOCKET_PATH"' EXIT
for attempt in $(seq 1 50); do
  [ -S "$SHARED_SOCKET_PATH" ] && break
  sleep 0.1
done
[ -S "$SHARED_SOCKET_PATH" ]
python3 - "$SHARED_SOCKET_PATH" "$OUT_DIR/failure.jsonl" "$OUT_DIR/recovery.jsonl" <<'PY'
import socket
import sys

path, first, second = sys.argv[1:]
for filename in (first, second):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(path)
        with open(filename, "rb") as source:
            client.sendall(source.read())
PY
wait "$SHARED_DAEMON_PID"
grep -q 'socket_mode: parallel-shared-batch' "$OUT_DIR/shared.log"
grep -q 'selected_path: path-via-hub' "$OUT_DIR/shared.log"
grep -q 'selected_path: path-direct' "$OUT_DIR/shared.log"
[ ! -e "$SHARED_SOCKET_PATH" ]
trap - EXIT

TIMEOUT_SOCKET_PATH="$OUT_DIR/eventnetd-timeout.sock"
TIMEOUT_STATE_FILE="$OUT_DIR/timeout-state.tsv"
rm -f "$TIMEOUT_SOCKET_PATH" "$TIMEOUT_STATE_FILE"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-socket "$TIMEOUT_SOCKET_PATH" --socket-accept-count 2 --socket-parallel \
  --socket-parallel-timeout-ms 500 --state-file "$TIMEOUT_STATE_FILE" > "$OUT_DIR/timeout.log" 2>&1 &
TIMEOUT_DAEMON_PID=$!
trap 'kill "$TIMEOUT_DAEMON_PID" 2>/dev/null || true; rm -f "$TIMEOUT_SOCKET_PATH"' EXIT
for attempt in $(seq 1 50); do
  [ -S "$TIMEOUT_SOCKET_PATH" ] && break
  sleep 0.1
done
[ -S "$TIMEOUT_SOCKET_PATH" ]
python3 - "$TIMEOUT_SOCKET_PATH" <<'PY'
import socket
import sys
import time

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as first:
    first.connect(sys.argv[1])
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as second:
        second.connect(sys.argv[1])
        time.sleep(1)
PY
if wait "$TIMEOUT_DAEMON_PID"; then
  printf '%s\n' 'parallel timeout unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'telemetry parallel receive timed out after 500 ms' "$OUT_DIR/timeout.log"
[ ! -e "$TIMEOUT_SOCKET_PATH" ]
trap - EXIT

printf '%s\n' '{"schema":"ibuki.event.path.v1","event":"path_failed","path_id":"path-direct"' > "$OUT_DIR/malformed.jsonl"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/malformed.jsonl" --state-file "$OUT_DIR/malformed-state.tsv" > "$OUT_DIR/malformed.log" 2>&1; then
  printf '%s\n' 'malformed telemetry unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'telemetry parse failed' "$OUT_DIR/malformed.log"
[ ! -e "$OUT_DIR/malformed-state.tsv" ]

printf '%s\n' 'not-json' > "$OUT_DIR/missing-schema.jsonl"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/missing-schema.jsonl" > "$OUT_DIR/missing-schema.log" 2>&1; then
  printf '%s\n' 'missing-schema telemetry unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'telemetry load failed' "$OUT_DIR/missing-schema.log"

long_option_value=$(awk 'BEGIN { for (index = 0; index < 256; index++) printf "x"; printf "\n" }')
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --telemetry "$OUT_DIR/recovery.jsonl" \
  --swanctl-uri "$long_option_value" > "$OUT_DIR/long-uri.log" 2>&1; then
  printf '%s\n' 'overlong swanctl URI was unexpectedly accepted' >&2
  exit 1
fi
grep -q 'invalid --swanctl-uri' "$OUT_DIR/long-uri.log"

if "$BUILD_DIR/eventnetd" --intent > "$OUT_DIR/missing-intent.log" 2>&1; then
  printf '%s\n' 'missing intent value was unexpectedly accepted' >&2
  exit 1
fi
grep -q 'missing --intent value' "$OUT_DIR/missing-intent.log"

printf '%s\n' '{"schema":"ibuki.telemetry.path_health.v2","path_id":"path-direct","state":"healthy","rtt_ms":1,"packet_loss_percent":0,"jitter_ms":0,"timestamp_ms":1}' > "$OUT_DIR/unknown-schema.jsonl"
if "$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/unknown-schema.jsonl" > "$OUT_DIR/unknown-schema.log" 2>&1; then
  printf '%s\n' 'unknown-schema telemetry unexpectedly succeeded' >&2
  exit 1
fi
grep -q 'telemetry load failed' "$OUT_DIR/unknown-schema.log"

SHUTDOWN_STATE_FILE="$OUT_DIR/shutdown-state.tsv"
SHUTDOWN_DAEMON_LOG="$OUT_DIR/shutdown.log"
rm -f "$SHUTDOWN_STATE_FILE"
"$BUILD_DIR/eventnetd" samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry "$OUT_DIR/recovery.jsonl" --reload-config --state-file "$SHUTDOWN_STATE_FILE" \
  --interval-ms 100 --count 1000 > "$SHUTDOWN_DAEMON_LOG" 2>&1 &
SHUTDOWN_DAEMON_PID=$!
trap 'kill "$SHUTDOWN_DAEMON_PID" 2>/dev/null || true; rm -f "$SHUTDOWN_STATE_FILE"' EXIT
sleep 0.3
kill -TERM "$SHUTDOWN_DAEMON_PID"
wait "$SHUTDOWN_DAEMON_PID"
test -s "$SHUTDOWN_STATE_FILE"
grep -q 'config_reload: applied' "$SHUTDOWN_DAEMON_LOG"
trap - EXIT

printf 'EventNet event smoke passed: failure fallback and recovery were reconciled.\n'
