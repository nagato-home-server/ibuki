#!/usr/bin/env sh
set -eu

RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

require_ns() {
  ns="$1"
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns-setup.sh first.\n' "$ns" >&2
    exit 1
  }
}

require_socket() {
  ns="$1"
  socket_path="$RUN_BASE/$ns/charon.vici"
  if [ ! -S "$socket_path" ]; then
    printf 'missing VICI socket: %s. Run sudo sh scripts/vm-netns-ipsec.sh hub start first.\n' "$socket_path" >&2
    exit 1
  fi
}

ensure_dummy_lan() {
  ns="$1"
  addr="$2"

  ip netns exec "$ns" ip link show lan0 >/dev/null 2>&1 || \
    ip netns exec "$ns" ip link add lan0 type dummy
  ip netns exec "$ns" ip addr flush dev lan0
  ip netns exec "$ns" ip addr add "$addr" dev lan0
  ip netns exec "$ns" ip link set lan0 up
}

ensure_hub_routes() {
  ip netns exec site-a ip route replace 10.10.2.0/24 dev xfrm-a-hub
  ip netns exec hub-1 ip route replace 10.10.1.0/24 dev xfrm-a-hub
  ip netns exec hub-1 ip route replace 10.10.2.0/24 dev xfrm-hub-b
  ip netns exec site-b ip route replace 10.10.1.0/24 dev xfrm-hub-b
}

child_out_packets() {
  ns="$1"
  child="$2"
  swanctl --list-sas --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null | \
    awk -v child="$child" '
      $1 == child ":" { in_child = 1; next }
      in_child && /^[^[:space:]]/ { in_child = 0 }
      in_child && /^[[:space:]]+out[[:space:]]/ {
        for (i = 1; i <= NF; i++) {
          if ($i ~ /^packets,?$/) {
            packets = $(i - 1)
            gsub(",", "", packets)
            print packets
            found = 1
            exit
          }
        }
      }
      END { if (!found) print 0 }
    '
}

for ns in site-a hub-1 site-b; do
  require_ns "$ns"
  require_socket "$ns"
done

ensure_dummy_lan site-a 10.10.1.1/24
ensure_dummy_lan site-b 10.10.2.1/24
ensure_hub_routes

before_a_hub=$(child_out_packets site-a tun-a-hub)
before_hub_b=$(child_out_packets hub-1 tun-hub-b)

printf '== encrypted hub ping: site-a -> hub-1 -> site-b ==\n'
ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1

after_a_hub=$(child_out_packets site-a tun-a-hub)
after_hub_b=$(child_out_packets hub-1 tun-hub-b)

printf '\nESP out packets:\n'
printf '  site-a/tun-a-hub: %s -> %s\n' "$before_a_hub" "$after_a_hub"
printf '  hub-1/tun-hub-b: %s -> %s\n' "$before_hub_b" "$after_hub_b"

if [ "$after_a_hub" -le "$before_a_hub" ]; then
  printf 'ESP counter did not increase on site-a/tun-a-hub.\n' >&2
  swanctl --list-sas --uri "unix://$RUN_BASE/site-a/charon.vici" >&2 || true
  exit 1
fi

if [ "$after_hub_b" -le "$before_hub_b" ]; then
  printf 'ESP counter did not increase on hub-1/tun-hub-b.\n' >&2
  swanctl --list-sas --uri "unix://$RUN_BASE/hub-1/charon.vici" >&2 || true
  exit 1
fi

printf '\nHub IPsec smoke passed: ping succeeded and both hub-path ESP counters increased.\n'
