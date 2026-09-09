#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-direct}"
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-direct}"
CHARON="${CHARON:-/usr/lib/ipsec/charon}"
USE_DEFAULT_STRONGSWAN_CONF="${USE_DEFAULT_STRONGSWAN_CONF:-0}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

if [ ! -x "$CHARON" ]; then
  printf 'charon not executable: %s\n' "$CHARON" >&2
  exit 1
fi

for ns in site-a site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns-setup.sh first.\n' "$ns" >&2
    exit 1
  }
done

sh "$ROOT_DIR/scripts/vm-netns-ipsec.sh" direct generate

for ns in site-a site-b; do
  pid_file="$RUN_BASE/$ns/charon.pid"
  if [ -s "$pid_file" ]; then
    old_pid=$(cat "$pid_file")
    kill "$old_pid" 2>/dev/null || true
  fi
  wrapper_pid_file="$RUN_BASE/$ns/eventnet-wrapper.pid"
  if [ -s "$wrapper_pid_file" ]; then
    old_wrapper_pid=$(cat "$wrapper_pid_file")
    kill "$old_wrapper_pid" 2>/dev/null || true
  fi
  for namespace_pid in $(ip netns pids "$ns" 2>/dev/null || true); do
    kill "$namespace_pid" 2>/dev/null || true
  done
done

prepare_node() {
  ns="$1"
  node_dir="$OUT_DIR/$ns"
  run_dir="$RUN_BASE/$ns"
  swanctl_work_dir="$SWANCTL_WORK_BASE/$ns"
  log_file="$run_dir/charon.log"

  mkdir -p "$run_dir" "$node_dir" "$swanctl_work_dir"
  rm -f "$run_dir/charon.pid" "$run_dir/eventnet-wrapper.pid" "$run_dir/charon.vici" "$log_file"
  rm -f "$swanctl_work_dir/swanctl.conf"
  chmod 755 "$RUN_BASE" "$run_dir" "$SWANCTL_WORK_BASE" "$swanctl_work_dir"
  for dir in x509 x509ca x509ocsp x509aa x509ac x509crl pubkey private rsa ecdsa bliss pkcs8 pkcs12; do
    mkdir -p "$run_dir/$dir"
    mkdir -p "$swanctl_work_dir/$dir"
    chmod 755 "$run_dir/$dir" "$swanctl_work_dir/$dir"
  done
}

start_node() {
  ns="$1"
  run_dir="$RUN_BASE/$ns"
  log_file="$run_dir/charon.log"

  if [ -s "$run_dir/charon.pid" ] && kill -0 "$(cat "$run_dir/charon.pid")" 2>/dev/null; then
    printf '%s charon already running: pid %s\n' "$ns" "$(cat "$run_dir/charon.pid")"
    return
  fi

  printf 'Starting charon in namespace %s...\n' "$ns"
  ip netns exec "$ns" unshare -m -- sh -c "mount --bind '$run_dir' /run && '$CHARON'" >"$log_file" 2>&1 &
  echo "$!" > "$run_dir/eventnet-wrapper.pid"

  for _ in 1 2 3 4 5 6 7 8 9 10; do
    socket_path="$run_dir/charon.vici"
    if [ -S "$socket_path" ] && swanctl --stats --uri "unix://$socket_path" >/dev/null 2>&1; then
      printf '%s VICI socket ready: %s\n' "$ns" "$socket_path"
      if [ -s "$run_dir/charon.pid" ]; then
        printf '%s charon pid: %s\n' "$ns" "$(cat "$run_dir/charon.pid")"
      fi
      return
    fi
    sleep 0.5
  done

  printf 'charon did not create VICI socket for %s. Log follows:\n' "$ns" >&2
  cat "$log_file" >&2 || true
  printf 'charon-related namespace state for %s:\n' "$ns" >&2
  ip netns pids "$ns" >&2 || true
  printf 'runtime directory for %s:\n' "$ns" >&2
  ls -la "$run_dir" >&2 || true
  exit 1
}

load_node() {
  ns="$1"
  node_dir="$OUT_DIR/$ns"
  run_dir="$RUN_BASE/$ns"
  swanctl_work_dir="$SWANCTL_WORK_BASE/$ns"
  swanctl_conf="$swanctl_work_dir/swanctl.conf"
  uri="unix://$run_dir/charon.vici"

  cp "$node_dir/swanctl.conf" "$swanctl_conf"
  chmod 600 "$swanctl_conf"
  if [ ! -r "$swanctl_conf" ]; then
    printf 'swanctl config is not readable: %s\n' "$swanctl_conf" >&2
    exit 1
  fi
  printf 'Loading swanctl config for %s...\n' "$ns"
  swanctl --load-conns --uri "$uri" --file "$swanctl_conf"
  swanctl --load-creds --uri "$uri" --file "$swanctl_conf"
  if ! swanctl --list-conns --uri "$uri" 2>/dev/null | grep -q 'tun-a-b'; then
    printf 'swanctl did not load tun-a-b for %s. Config was not printed because it contains secrets: %s\n' "$ns" "$swanctl_conf" >&2
    exit 1
  fi
}

initiate_direct() {
  printf 'Initiating direct tunnel from site-a...\n'
  run_dir="$RUN_BASE/site-a"
  swanctl --initiate --uri "unix://$run_dir/charon.vici" --child tun-a-b
}

prepare_node site-a
prepare_node site-b
start_node site-a
start_node site-b
load_node site-a
load_node site-b
initiate_direct

printf '\nDirect IPsec attempt completed. Inspect with:\n'
printf '  sudo sh scripts/vm-netns-ipsec.sh direct status\n'
printf '  sudo swanctl --list-sas --uri unix://%s/site-a/charon.vici\n' "$RUN_BASE"
printf '  sudo swanctl --list-sas --uri unix://%s/site-b/charon.vici\n' "$RUN_BASE"
