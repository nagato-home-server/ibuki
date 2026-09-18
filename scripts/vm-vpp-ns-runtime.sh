#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VPP=${VPP:-vpp}
VPPCTL=${VPPCTL:-vppctl}
RUN_BASE=${VPP_NS_RUN_BASE:-/run/ibuki-vpp-ns}
NODES=${VPP_NS_NODES:-site-a site-b}
ACTION=${1:-}

usage() {
  printf 'Usage: sudo %s start|stop|status\n' "$0" >&2
  exit 2
}

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s %s\n' "$0" "$ACTION" >&2
  exit 1
fi

command -v ip >/dev/null 2>&1 || { printf 'missing: ip\n' >&2; exit 1; }
command -v "$VPP" >/dev/null 2>&1 || { printf 'missing: %s\n' "$VPP" >&2; exit 1; }
command -v "$VPPCTL" >/dev/null 2>&1 || { printf 'missing: %s\n' "$VPPCTL" >&2; exit 1; }

config_for() {
  ns="$1"
  dir="$RUN_BASE/$ns"
  mkdir -p "$dir"
  chmod 755 "$RUN_BASE" "$dir"
  cat > "$dir/startup.conf" <<EOF
unix {
  nodaemon
  cli-listen $dir/cli.sock
  log $dir/vpp.log
}
api-segment {
  prefix ibuki-$ns
}
socksvr {
  socket-name $dir/api.sock
}
statseg {
  socket-name $dir/stats.sock
}
logging {
  default-log-level notice
}
plugins {
  plugin default { disable }
  plugin af_packet_plugin.so { enable }
  plugin gre_plugin.so { enable }
  plugin ipsec_plugin.so { enable }
  plugin ipsec_gre_plugin.so { enable }
}
EOF
}

is_running() {
  ns="$1"
  pid_file="$RUN_BASE/$ns/vpp.pid"
  [ -s "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

start_node() {
  ns="$1"
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s\n' "$ns" >&2
    exit 1
  }
  config_for "$ns"
  if is_running "$ns"; then
    printf '%s VPP already running: %s\n' "$ns" "$(cat "$RUN_BASE/$ns/vpp.pid")"
    return
  fi
  rm -f "$RUN_BASE/$ns/cli.sock" "$RUN_BASE/$ns/api.sock" "$RUN_BASE/$ns/stats.sock" "$RUN_BASE/$ns/vpp.pid"
  : > "$RUN_BASE/$ns/vpp.stdout.log"
  : > "$RUN_BASE/$ns/vpp.stderr.log"
  printf 'Starting VPP in namespace %s...\n' "$ns"
  ip netns exec "$ns" "$VPP" -c "$RUN_BASE/$ns/startup.conf" \
    >"$RUN_BASE/$ns/vpp.stdout.log" 2>"$RUN_BASE/$ns/vpp.stderr.log" &
  echo "$!" > "$RUN_BASE/$ns/vpp.pid"
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if [ -S "$RUN_BASE/$ns/cli.sock" ] && "$VPPCTL" -s "$RUN_BASE/$ns/cli.sock" show version >/dev/null 2>&1; then
      printf '%s VPP ready: %s\n' "$ns" "$RUN_BASE/$ns/cli.sock"
      return
    fi
    if ! is_running "$ns"; then
      break
    fi
    printf 'Waiting for VPP in %s...\n' "$ns"
    sleep 0.5
  done
  printf 'VPP did not become ready in %s. Log follows:\n' "$ns" >&2
  cat "$RUN_BASE/$ns/vpp.log" >&2 2>/dev/null || true
  printf '%s stdout:\n' "$ns" >&2
  cat "$RUN_BASE/$ns/vpp.stdout.log" >&2 2>/dev/null || true
  printf '%s stderr:\n' "$ns" >&2
  cat "$RUN_BASE/$ns/vpp.stderr.log" >&2 2>/dev/null || true
  printf '%s startup.conf:\n' "$ns" >&2
  cat "$RUN_BASE/$ns/startup.conf" >&2 2>/dev/null || true
  exit 1
}

stop_node() {
  ns="$1"
  pid_file="$RUN_BASE/$ns/vpp.pid"
  if [ -s "$pid_file" ]; then
    pid=$(cat "$pid_file")
    printf 'Stopping %s VPP pid %s\n' "$ns" "$pid"
    kill "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    kill -KILL "$pid" 2>/dev/null || true
  fi
  rm -f "$RUN_BASE/$ns/cli.sock" "$RUN_BASE/$ns/api.sock" "$RUN_BASE/$ns/stats.sock" "$RUN_BASE/$ns/vpp.pid"
}

status_node() {
  ns="$1"
  if is_running "$ns" && "$VPPCTL" -s "$RUN_BASE/$ns/cli.sock" show version; then
    printf '%s: active\n' "$ns"
  else
    printf '%s: inactive\n' "$ns"
  fi
}

case "$ACTION" in
  start)
    for ns in $NODES; do start_node "$ns"; done
    ;;
  stop)
    for ns in $NODES; do stop_node "$ns"; done
    ;;
  status)
    for ns in $NODES; do status_node "$ns"; done
    ;;
  *) usage ;;
esac
