#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-hub}"
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-hub}"
DIRECT_RUN_BASE="${DIRECT_RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
CHARON="${CHARON:-/usr/lib/ipsec/charon}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

if [ ! -x "$CHARON" ]; then
  printf 'charon not executable: %s\n' "$CHARON" >&2
  exit 1
fi

for ns in site-a hub-1 site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns-setup.sh first.\n' "$ns" >&2
    exit 1
  }
  if [ -s "$DIRECT_RUN_BASE/$ns/charon.pid" ] && kill -0 "$(cat "$DIRECT_RUN_BASE/$ns/charon.pid")" 2>/dev/null; then
    printf 'direct IPsec charon is still running in %s. Run sudo sh scripts/vm-netns-ipsec.sh direct stop first.\n' "$ns" >&2
    exit 1
  fi
done

sh "$ROOT_DIR/scripts/vm-netns-ipsec.sh" hub generate

for ns in site-a hub-1 site-b; do
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
    mkdir -p "$run_dir/$dir" "$swanctl_work_dir/$dir"
    chmod 755 "$run_dir/$dir" "$swanctl_work_dir/$dir"
  done
}

start_node() {
  ns="$1"
  run_dir="$RUN_BASE/$ns"
  log_file="$run_dir/charon.log"

  printf 'Starting charon in namespace %s...\n' "$ns"
  ip netns exec "$ns" unshare -m -- sh -c "mount --bind '$run_dir' /run && '$CHARON'" >/dev/null 2>>"$log_file" &
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
  exit 1
}

load_node() {
  ns="$1"
  expected="$2"
  node_dir="$OUT_DIR/$ns"
  run_dir="$RUN_BASE/$ns"
  swanctl_work_dir="$SWANCTL_WORK_BASE/$ns"
  swanctl_conf="$swanctl_work_dir/swanctl.conf"
  uri="unix://$run_dir/charon.vici"

  cp "$node_dir/swanctl.conf" "$swanctl_conf"
  chmod 600 "$swanctl_conf"
  printf 'Loading swanctl config for %s...\n' "$ns"
  swanctl --load-conns --uri "$uri" --file "$swanctl_conf"
  swanctl --load-creds --uri "$uri" --file "$swanctl_conf"
  for child in $expected; do
    if ! swanctl --list-conns --uri "$uri" 2>/dev/null | grep -q "$child"; then
      printf 'swanctl did not load %s for %s. Config was not printed because it contains secrets: %s\n' "$child" "$ns" "$swanctl_conf" >&2
      exit 1
    fi
  done
}

add_xfrmi() {
  ns="$1"
  name="$2"
  dev="$3"
  if_id="$4"

  ip netns exec "$ns" ip link del "$name" 2>/dev/null || true
  ip netns exec "$ns" ip link add "$name" type xfrm dev "$dev" if_id "$if_id"
  ip netns exec "$ns" ip link set "$name" up
}

configure_routes() {
  ip netns exec hub-1 sysctl -w net.ipv4.ip_forward=1 >/dev/null

  add_xfrmi site-a xfrm-a-hub a-hub 101
  add_xfrmi hub-1 xfrm-a-hub hub-a 101
  add_xfrmi hub-1 xfrm-hub-b hub-b 102
  add_xfrmi site-b xfrm-hub-b b-hub 102

  ip netns exec site-a ip route replace 10.10.2.0/24 dev xfrm-a-hub
  ip netns exec hub-1 ip route replace 10.10.1.0/24 dev xfrm-a-hub
  ip netns exec hub-1 ip route replace 10.10.2.0/24 dev xfrm-hub-b
  ip netns exec site-b ip route replace 10.10.1.0/24 dev xfrm-hub-b
}

initiate_hub() {
  printf 'Initiating hub tunnels...\n'
  swanctl --initiate --uri "unix://$RUN_BASE/site-a/charon.vici" --child tun-a-hub
  swanctl --initiate --uri "unix://$RUN_BASE/hub-1/charon.vici" --child tun-hub-b
}

sh "$ROOT_DIR/scripts/vm-netns-ipsec.sh" hub clean
prepare_node site-a
prepare_node hub-1
prepare_node site-b
start_node site-a
start_node hub-1
start_node site-b
load_node site-a "tun-a-hub"
load_node hub-1 "tun-a-hub tun-hub-b"
load_node site-b "tun-hub-b"
configure_routes
initiate_hub

printf '\nHub IPsec attempt completed. Inspect with:\n'
printf '  sudo sh scripts/vm-netns-ipsec.sh hub status\n'
printf '  sudo sh scripts/vm-netns-ipsec.sh hub smoke\n'
