#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${1:-${GRE_OUT_DIR:-$ROOT_DIR/out/netns-runtime}}"
RUN_BASE="${GRE_RUN_BASE:-/run/eventnet-netns-ipsec-gre}"
SWANCTL_WORK_BASE="${GRE_SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-gre}"
CHARON="${CHARON:-/usr/lib/ipsec/charon}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s [out-dir]\n' "$0" >&2
  exit 1
fi

for ns in site-a site-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s\n' "$ns" >&2
    exit 1
  }
done

CONFIG="$OUT_DIR/gre-swanctl.conf"
[ -r "$CONFIG" ] || { printf 'missing GRE swanctl config: %s\n' "$CONFIG" >&2; exit 1; }
[ -x "$CHARON" ] || { printf 'charon not executable: %s\n' "$CHARON" >&2; exit 1; }

local_endpoint="${GRE_IKE_LOCAL_ENDPOINT:-$(awk '$1 == "local_addrs" { print $3; exit }' "$CONFIG")}"
remote_endpoint="${GRE_IKE_REMOTE_ENDPOINT:-$(awk '$1 == "remote_addrs" { print $3; exit }' "$CONFIG")}"
local_id="${GRE_LOCAL_ID:-$(awk '$1 == "id" { print $3; exit }' "$CONFIG")}"
remote_id="${GRE_REMOTE_ID:-$(awk '$1 == "id" { count++; if (count == 2) { print $3; exit } }' "$CONFIG")}"
outer_local="${GRE_OUTER_LOCAL_ENDPOINT:-172.16.1.1}"
outer_remote="${GRE_OUTER_REMOTE_ENDPOINT:-172.16.2.1}"

ip netns exec site-a sysctl -w net.ipv4.ip_forward=1 >/dev/null
ip netns exec site-b sysctl -w net.ipv4.ip_forward=1 >/dev/null
ip netns exec site-a ip route replace "$outer_remote/32" via "$remote_endpoint" dev a-direct
ip netns exec site-b ip route replace "$outer_local/32" via "$local_endpoint" dev b-direct

mkdir -p "$RUN_BASE/site-a" "$RUN_BASE/site-b" "$SWANCTL_WORK_BASE/site-a" "$SWANCTL_WORK_BASE/site-b"
chmod 755 "$RUN_BASE" "$RUN_BASE/site-a" "$RUN_BASE/site-b" "$SWANCTL_WORK_BASE" "$SWANCTL_WORK_BASE/site-a" "$SWANCTL_WORK_BASE/site-b"

stop_existing() {
  ns="$1"
  run_dir="$RUN_BASE/$ns"
  for pid_file in "$run_dir/charon.pid" "$run_dir/eventnet-wrapper.pid"; do
    if [ -s "$pid_file" ]; then kill "$(cat "$pid_file")" 2>/dev/null || true; fi
  done
  for namespace_pid in $(ip netns pids "$ns" 2>/dev/null || true); do kill "$namespace_pid" 2>/dev/null || true; done
  rm -f "$run_dir/charon.pid" "$run_dir/eventnet-wrapper.pid" "$run_dir/charon.vici" "$run_dir/charon.log"
}

start_node() {
  ns="$1"
  run_dir="$RUN_BASE/$ns"
  log_file="$run_dir/charon.log"
  printf 'Starting GRE charon in namespace %s...\n' "$ns"
  ip netns exec "$ns" unshare -m -- sh -c "mount --bind '$run_dir' /run && '$CHARON'" >"$log_file" 2>&1 &
  echo "$!" > "$run_dir/eventnet-wrapper.pid"
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if [ -S "$run_dir/charon.vici" ] && swanctl --stats --uri "unix://$run_dir/charon.vici" >/dev/null 2>&1; then
      printf '%s GRE VICI ready: %s\n' "$ns" "$run_dir/charon.vici"
      return
    fi
    sleep 0.5
  done
  cat "$log_file" >&2 || true
  exit 1
}

stop_existing site-a
stop_existing site-b
cp "$CONFIG" "$SWANCTL_WORK_BASE/site-a/swanctl.conf"
sed \
  -e "s/^    local_addrs = .*/    local_addrs = $remote_endpoint/" \
  -e "s/^    remote_addrs = .*/    remote_addrs = $local_endpoint/" \
  -e "s/^      id = $local_id$/      id = __IBUKI_LOCAL_ID__/" \
  -e "s/^      id = $remote_id$/      id = $local_id/" \
  -e "s/^      id = __IBUKI_LOCAL_ID__$/      id = $remote_id/" \
  -e "s/^    id-1 = $local_id$/    id-1 = __IBUKI_LOCAL_ID__/" \
  -e "s/^    id-2 = $remote_id$/    id-2 = $local_id/" \
  -e "s/^    id-1 = __IBUKI_LOCAL_ID__$/    id-1 = $remote_id/" \
  "$CONFIG" > "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
chmod 600 "$SWANCTL_WORK_BASE/site-a/swanctl.conf" "$SWANCTL_WORK_BASE/site-b/swanctl.conf"

start_node site-a
start_node site-b
swanctl --load-conns --uri "unix://$RUN_BASE/site-a/charon.vici" --file "$SWANCTL_WORK_BASE/site-a/swanctl.conf"
swanctl --load-creds --uri "unix://$RUN_BASE/site-a/charon.vici" --file "$SWANCTL_WORK_BASE/site-a/swanctl.conf"
swanctl --load-conns --uri "unix://$RUN_BASE/site-b/charon.vici" --file "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
swanctl --load-creds --uri "unix://$RUN_BASE/site-b/charon.vici" --file "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
swanctl --initiate --uri "unix://$RUN_BASE/site-a/charon.vici" --child gre-a-b

printf 'GRE over IPsec CHILD_SA established.\n'
