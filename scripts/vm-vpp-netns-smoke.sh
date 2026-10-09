#!/usr/bin/env sh
set -eu

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a site-b client-a client-b; do
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
    exit 1
  }
done

diagnose_failure() {
  label="$1"
  printf '\nVPP smoke failed at: %s\n' "$label" >&2
  for ns in site-a site-b; do
    printf '\n== %s Linux state ==\n' "$ns" >&2
    ip netns exec "$ns" ip -br addr >&2 || true
    ip netns exec "$ns" ip route >&2 || true
    ip netns exec "$ns" ip neigh >&2 || true
    printf '\n== %s VPP state ==\n' "$ns" >&2
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show interface >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show hardware-interfaces >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show interface address >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show ip fib >&2 || true
    vppctl -s "/run/ibuki-vpp-ns/$ns/cli.sock" show errors >&2 || true
  done
}

check_local_vpp_address() {
  ns="$1"
  source_address="$2"
  destination_address="$3"
  if ip netns exec "$ns" ping -c 2 -I "$source_address" "$destination_address"; then
    return 0
  fi
  printf 'warning: %s did not answer its VPP local address; continuing to data-plane checks\n' "$ns" >&2
  if [ "${REQUIRE_LOCAL_VPP_PING:-0}" = "1" ]; then
    diagnose_failure "$ns local VPP interface"
    return 1
  fi
  return 0
}

printf '== client-a -> local VPP interface ==\n'
check_local_vpp_address client-a 10.10.1.2 10.10.1.1

printf '\n== client-b -> local VPP interface ==\n'
check_local_vpp_address client-b 10.10.2.2 10.10.2.1

printf '\n== client-a LAN -> client-b LAN via VPP ==\n'
if ! ip netns exec client-a ping -c 3 -I 10.10.1.2 10.10.2.2; then
  diagnose_failure 'client-a to client-b through VPP'
  exit 1
fi

printf '\n== client-b LAN -> client-a LAN via VPP ==\n'
if ! ip netns exec client-b ping -c 3 -I 10.10.2.2 10.10.1.2; then
  diagnose_failure 'client-b to client-a through VPP'
  exit 1
fi

printf '\nVPP netns smoke passed: site-a and site-b reached each other through VPP host interfaces.\n'
