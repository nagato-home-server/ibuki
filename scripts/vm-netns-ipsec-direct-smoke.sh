#!/usr/bin/env sh
set -eu

RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"

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

ensure_dummy_lan() {
  ns="$1"
  addr="$2"

  ip netns exec "$ns" ip link show lan0 >/dev/null 2>&1 || \
    ip netns exec "$ns" ip link add lan0 type dummy
  ip netns exec "$ns" ip addr flush dev lan0
  ip netns exec "$ns" ip addr add "$addr" dev lan0
  ip netns exec "$ns" ip link set lan0 up
}

ensure_direct_routes() {
  ip netns exec site-a ip route replace 10.10.2.0/24 via 203.0.113.9
  ip netns exec site-b ip route replace 10.10.1.0/24 via 203.0.113.10
}

require_socket() {
  ns="$1"
  socket_path="$RUN_BASE/$ns/charon.vici"
  if [ ! -S "$socket_path" ]; then
    printf 'missing VICI socket: %s. Run sudo sh scripts/vm-netns-ipsec.sh direct start first.\n' "$socket_path" >&2
    exit 1
  fi
}

out_packets() {
  ns="$1"
  swanctl --list-sas --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null | \
    awk '
      /^[[:space:]]+out[[:space:]]/ {
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

for ns in site-a site-b; do
  require_ns "$ns"
  require_socket "$ns"
done

ensure_dummy_lan site-a 10.10.1.1/24
ensure_dummy_lan site-b 10.10.2.1/24
ensure_direct_routes

before_a=$(out_packets site-a)
before_b=$(out_packets site-b)

printf '== encrypted direct ping: site-a -> site-b ==\n'
ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1

after_a=$(out_packets site-a)
after_b=$(out_packets site-b)

printf '\nESP out packets:\n'
printf '  site-a: %s -> %s\n' "$before_a" "$after_a"
printf '  site-b: %s -> %s\n' "$before_b" "$after_b"

if [ "$after_a" -le "$before_a" ]; then
  printf 'ESP counter did not increase on site-a. IPsec may not be carrying the ping.\n' >&2
  printf '\nsite-a SAs:\n' >&2
  swanctl --list-sas --uri "unix://$RUN_BASE/site-a/charon.vici" >&2 || true
  exit 1
fi

if [ "$after_b" -le "$before_b" ]; then
  printf 'ESP counter did not increase on site-b. Reply traffic may not be protected.\n' >&2
  printf '\nsite-b SAs:\n' >&2
  swanctl --list-sas --uri "unix://$RUN_BASE/site-b/charon.vici" >&2 || true
  exit 1
fi

printf '\nIPsec direct smoke passed: ping succeeded and ESP counters increased.\n'
