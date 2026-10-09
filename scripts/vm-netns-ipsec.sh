#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
  cat >&2 <<'EOF'
Usage:
  sh scripts/vm-netns-ipsec.sh direct|hub generate
  sudo sh scripts/vm-netns-ipsec.sh gre start [out-dir]
  sudo sh scripts/vm-netns-ipsec.sh direct|hub start
  sudo sh scripts/vm-netns-ipsec.sh direct|hub status
  sh scripts/vm-netns-ipsec.sh direct|hub logs
  sudo sh scripts/vm-netns-ipsec.sh direct|hub smoke
  sudo sh scripts/vm-netns-ipsec.sh direct|hub stop
  sudo sh scripts/vm-netns-ipsec.sh direct|hub clean
EOF
  exit 1
}

need_root() {
  if [ "$(id -u)" != "0" ]; then
    printf 'Please run as root: sudo %s %s %s\n' "$0" "$MODE" "$ACTION" >&2
    exit 1
  fi
}

MODE="${1:-}"
ACTION="${2:-}"

[ -n "$MODE" ] && [ -n "$ACTION" ] || usage
shift 2
case "$MODE:$ACTION" in
  direct:generate|direct:start|direct:smoke|direct:stop|direct:clean|direct:logs|direct:status|hub:generate|hub:start|hub:smoke|hub:stop|hub:clean|hub:logs|hub:status|gre:start|gre:stop|gre:clean|gre:logs|gre:status) ;;
  *) usage ;;
esac

case "$MODE" in
  direct)
    NODES="site-a site-b"
    RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
    SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-direct}"
    ;;
  hub)
    NODES="site-a hub-1 site-b"
    RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"
    SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-hub}"
    ;;
  gre)
    NODES="site-a site-b"
    RUN_BASE="${GRE_RUN_BASE:-${RUN_BASE:-/run/eventnet-netns-ipsec-gre}}"
    SWANCTL_WORK_BASE="${GRE_SWANCTL_WORK_BASE:-${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-gre}}"
    ;;
  *)
    usage
    ;;
esac

run_existing() {
  operation="ipsec_${MODE}_$1"
  shift
  "$operation" "$@"
}

clean_direct() {
  need_root
  for ns in site-a site-b; do
    ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
    ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
  done
  printf 'Flushed xfrm state and policy in site-a/site-b.\n'
}

clean_hub() {
  need_root
  ip netns exec site-a ip route del 10.10.2.0/24 2>/dev/null || true
  ip netns exec hub-1 ip route del 10.10.1.0/24 2>/dev/null || true
  ip netns exec hub-1 ip route del 10.10.2.0/24 2>/dev/null || true
  ip netns exec site-b ip route del 10.10.1.0/24 2>/dev/null || true

  for ns in site-a hub-1 site-b; do
    ip netns exec "$ns" ip link del xfrm-a-hub 2>/dev/null || true
    ip netns exec "$ns" ip link del xfrm-hub-b 2>/dev/null || true
    ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
    ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
  done
  printf 'Flushed hub XFRM state, policy, routes, and xfrm interfaces.\n'
}

clean_gre() {
  need_root
  sh "$ROOT_DIR/scripts/vm-netns-ipsec.sh" gre stop
}

show_logs() {
  lines=120
  [ "$MODE" = "hub" ] && lines=160
  for ns in $NODES; do
    printf '\n== %s charon.log ==\n' "$ns"
    if [ -f "$RUN_BASE/$ns/charon.log" ]; then
      tail -n "$lines" "$RUN_BASE/$ns/charon.log"
    else
      printf 'missing: %s\n' "$RUN_BASE/$ns/charon.log"
    fi
  done
}

show_status() {
  need_root
  for ns in $NODES; do
    printf '\n== %s ==\n' "$ns"
    ip netns exec "$ns" ip addr show
    if [ "$MODE" = "hub" ]; then
      printf '\n-- routes --\n'
      ip netns exec "$ns" ip route
      printf '\n-- xfrm links --\n'
      ip netns exec "$ns" ip -d link show type xfrm || true
    fi
    printf '\n-- xfrm state --\n'
    ip netns exec "$ns" ip xfrm state || true
    printf '\n-- xfrm policy --\n'
    ip netns exec "$ns" ip xfrm policy || true
    if [ -s "$RUN_BASE/$ns/charon.pid" ]; then
      printf '\ncharon pid: %s\n' "$(cat "$RUN_BASE/$ns/charon.pid")"
    fi
    if [ -f "$SWANCTL_WORK_BASE/$ns/swanctl.conf" ]; then
      printf 'swanctl work config: %s\n' "$SWANCTL_WORK_BASE/$ns/swanctl.conf"
      ls -ld "$SWANCTL_WORK_BASE" "$SWANCTL_WORK_BASE/$ns" "$SWANCTL_WORK_BASE/$ns/swanctl.conf"
    fi
    printf '\n-- swanctl conns --\n'
    swanctl --list-conns --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null || true
    printf '\n-- swanctl sas --\n'
    swanctl --list-sas --uri "unix://$RUN_BASE/$ns/charon.vici" 2>/dev/null || true
  done
}

ipsec_direct_generate() (
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-direct}"
PSK="${PSK:-}"

if [ -z "$PSK" ]; then
  PSK=$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')
elif [ "$PSK" = "change-me" ]; then
  printf 'Refusing insecure PSK value: change-me\n' >&2
  exit 1
fi

umask 077
mkdir -p "$OUT_DIR/site-a" "$OUT_DIR/site-b"

cat > "$OUT_DIR/site-a/swanctl.conf" <<EOF
connections {
  tun-a-b {
    version = 2
    local_addrs = 203.0.113.10
    remote_addrs = 203.0.113.9
    local {
      auth = psk
      id = site-a
    }
    remote {
      auth = psk
      id = site-b
    }
    children {
      tun-a-b {
        local_ts = 10.10.1.0/24
        remote_ts = 10.10.2.0/24
        mode = tunnel
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }
}

secrets {
  ike-tun-a-b {
    id-1 = site-a
    id-2 = site-b
    secret = $PSK
  }
}
EOF
chmod 600 "$OUT_DIR/site-a/swanctl.conf"

cat > "$OUT_DIR/site-b/swanctl.conf" <<EOF
connections {
  tun-a-b {
    version = 2
    local_addrs = 203.0.113.9
    remote_addrs = 203.0.113.10
    local {
      auth = psk
      id = site-b
    }
    remote {
      auth = psk
      id = site-a
    }
    children {
      tun-a-b {
        local_ts = 10.10.2.0/24
        remote_ts = 10.10.1.0/24
        mode = tunnel
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }
}

secrets {
  ike-tun-a-b {
    id-1 = site-b
    id-2 = site-a
    secret = $PSK
  }
}
EOF
chmod 600 "$OUT_DIR/site-b/swanctl.conf"

cat > "$OUT_DIR/README.txt" <<EOF
Generated direct IPsec configs:
  $OUT_DIR/site-a/swanctl.conf
  $OUT_DIR/site-b/swanctl.conf

Endpoints:
  site-a 203.0.113.10, protected LAN 10.10.1.0/24
  site-b 203.0.113.9,  protected LAN 10.10.2.0/24
EOF

printf 'Generated direct IPsec configs under %s\n' "$OUT_DIR"
)

ipsec_direct_start() (
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
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
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
    if [ -r "/proc/$namespace_pid/comm" ] && [ "$(cat "/proc/$namespace_pid/comm")" = "charon" ]; then
      kill "$namespace_pid" 2>/dev/null || true
    fi
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
)

ipsec_direct_smoke() (
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

require_ns() {
  ns="$1"
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
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
)

ipsec_direct_stop() (
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-direct}"
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-direct}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-direct}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a site-b; do
  pid_file="$RUN_BASE/$ns/charon.pid"
  wrapper_pid_file="$RUN_BASE/$ns/eventnet-wrapper.pid"
  if [ -s "$pid_file" ]; then
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
      printf 'Stopping %s charon pid %s\n' "$ns" "$pid"
      kill "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
  fi
  if [ -s "$wrapper_pid_file" ]; then
    wrapper_pid=$(cat "$wrapper_pid_file")
    kill "$wrapper_pid" 2>/dev/null || true
    rm -f "$wrapper_pid_file"
  fi
  for namespace_pid in $(ip netns pids "$ns" 2>/dev/null || true); do
    if [ -r "/proc/$namespace_pid/comm" ] && [ "$(cat "/proc/$namespace_pid/comm")" = "charon" ]; then
      kill "$namespace_pid" 2>/dev/null || true
    fi
  done
  rm -f "$RUN_BASE/$ns/charon.vici"
done

rm -rf "$SWANCTL_WORK_BASE"
for ns in site-a site-b; do
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
done

printf 'Flushed xfrm state and policy in site-a/site-b.\n'
)

ipsec_hub_generate() (
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/netns-ipsec-hub}"
PSK="${PSK:-}"

if [ -z "$PSK" ]; then
  PSK=$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')
elif [ "$PSK" = "change-me" ]; then
  printf 'Refusing insecure PSK value: change-me\n' >&2
  exit 1
fi

umask 077
mkdir -p "$OUT_DIR/site-a" "$OUT_DIR/hub-1" "$OUT_DIR/site-b"

cat > "$OUT_DIR/site-a/swanctl.conf" <<EOF
connections {
  tun-a-hub {
    version = 2
    local_addrs = 203.0.113.14
    remote_addrs = 203.0.113.13
    local {
      auth = psk
      id = site-a
    }
    remote {
      auth = psk
      id = hub-1
    }
    children {
      tun-a-hub {
        local_ts = 0.0.0.0/0
        remote_ts = 0.0.0.0/0
        mode = tunnel
        if_id_in = 101
        if_id_out = 101
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }
}

secrets {
  ike-tun-a-hub {
    id-1 = site-a
    id-2 = hub-1
    secret = $PSK
  }
}
EOF
chmod 600 "$OUT_DIR/site-a/swanctl.conf"

cat > "$OUT_DIR/hub-1/swanctl.conf" <<EOF
connections {
  tun-a-hub {
    version = 2
    local_addrs = 203.0.113.13
    remote_addrs = 203.0.113.14
    local {
      auth = psk
      id = hub-1
    }
    remote {
      auth = psk
      id = site-a
    }
    children {
      tun-a-hub {
        local_ts = 0.0.0.0/0
        remote_ts = 0.0.0.0/0
        mode = tunnel
        if_id_in = 101
        if_id_out = 101
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }

  tun-hub-b {
    version = 2
    local_addrs = 203.0.113.17
    remote_addrs = 203.0.113.18
    local {
      auth = psk
      id = hub-1
    }
    remote {
      auth = psk
      id = site-b
    }
    children {
      tun-hub-b {
        local_ts = 0.0.0.0/0
        remote_ts = 0.0.0.0/0
        mode = tunnel
        if_id_in = 102
        if_id_out = 102
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }
}

secrets {
  ike-tun-a-hub {
    id-1 = hub-1
    id-2 = site-a
    secret = $PSK
  }
  ike-tun-hub-b {
    id-1 = hub-1
    id-2 = site-b
    secret = $PSK
  }
}
EOF
chmod 600 "$OUT_DIR/hub-1/swanctl.conf"

cat > "$OUT_DIR/site-b/swanctl.conf" <<EOF
connections {
  tun-hub-b {
    version = 2
    local_addrs = 203.0.113.18
    remote_addrs = 203.0.113.17
    local {
      auth = psk
      id = site-b
    }
    remote {
      auth = psk
      id = hub-1
    }
    children {
      tun-hub-b {
        local_ts = 0.0.0.0/0
        remote_ts = 0.0.0.0/0
        mode = tunnel
        if_id_in = 102
        if_id_out = 102
        esp_proposals = aes128-sha256
        start_action = trap
      }
    }
    proposals = aes128-sha256-modp2048
  }
}

secrets {
  ike-tun-hub-b {
    id-1 = site-b
    id-2 = hub-1
    secret = $PSK
  }
}
EOF
chmod 600 "$OUT_DIR/site-b/swanctl.conf"

cat > "$OUT_DIR/README.txt" <<EOF
Generated hub IPsec configs:
  $OUT_DIR/site-a/swanctl.conf
  $OUT_DIR/hub-1/swanctl.conf
  $OUT_DIR/site-b/swanctl.conf

Route-based IPsec / XFRM interfaces:
  site-a xfrm-a-hub if_id 101
  hub-1  xfrm-a-hub if_id 101
  hub-1  xfrm-hub-b if_id 102
  site-b xfrm-hub-b if_id 102
EOF

printf 'Generated hub IPsec configs under %s\n' "$OUT_DIR"
)

ipsec_hub_start() (
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
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
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
)

ipsec_hub_smoke() (
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

require_ns() {
  ns="$1"
  ip netns exec "$ns" true >/dev/null 2>&1 || {
    printf 'namespace missing: %s. Run sudo sh scripts/vm-netns.sh setup first.\n' "$ns" >&2
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
)

ipsec_hub_stop() (
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RUN_BASE="${RUN_BASE:-/run/eventnet-netns-ipsec-hub}"
SWANCTL_WORK_BASE="${SWANCTL_WORK_BASE:-/etc/swanctl/eventnet-netns-ipsec-hub}"

if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

for ns in site-a hub-1 site-b; do
  pid_file="$RUN_BASE/$ns/charon.pid"
  wrapper_pid_file="$RUN_BASE/$ns/eventnet-wrapper.pid"
  if [ -s "$pid_file" ]; then
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
      printf 'Stopping %s hub charon pid %s\n' "$ns" "$pid"
      kill "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
  fi
  if [ -s "$wrapper_pid_file" ]; then
    wrapper_pid=$(cat "$wrapper_pid_file")
    kill "$wrapper_pid" 2>/dev/null || true
    rm -f "$wrapper_pid_file"
  fi
  rm -f "$RUN_BASE/$ns/charon.vici"
done

rm -rf "$SWANCTL_WORK_BASE"
ip netns exec site-a ip route del 10.10.2.0/24 2>/dev/null || true
ip netns exec hub-1 ip route del 10.10.1.0/24 2>/dev/null || true
ip netns exec hub-1 ip route del 10.10.2.0/24 2>/dev/null || true
ip netns exec site-b ip route del 10.10.1.0/24 2>/dev/null || true

for ns in site-a hub-1 site-b; do
  ip netns exec "$ns" ip link del xfrm-a-hub 2>/dev/null || true
  ip netns exec "$ns" ip link del xfrm-hub-b 2>/dev/null || true
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
done

printf 'Flushed hub XFRM state, policy, routes, and xfrm interfaces.\n'
)

ipsec_gre_start() (
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
child_id="${GRE_CHILD:-gre-a-b}"

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
  for namespace_pid in $(ip netns pids "$ns" 2>/dev/null || true); do
    if [ -r "/proc/$namespace_pid/comm" ] && [ "$(cat "/proc/$namespace_pid/comm")" = "charon" ]; then
      kill "$namespace_pid" 2>/dev/null || true
    fi
  done
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
  -e "s/^        local_ts = .*/        local_ts = $outer_remote\/32[gre]/" \
  -e "s/^        remote_ts = .*/        remote_ts = $outer_local\/32[gre]/" \
  -e "s/^      id = $local_id$/      id = __IBUKI_LOCAL_ID__/" \
  -e "s/^      id = $remote_id$/      id = $local_id/" \
  -e "s/^      id = __IBUKI_LOCAL_ID__$/      id = $remote_id/" \
  -e "s/^    id-1 = $local_id$/    id-1 = __IBUKI_LOCAL_ID__/" \
  -e "s/^    id-2 = $remote_id$/    id-2 = $local_id/" \
  -e "s/^    id-1 = __IBUKI_LOCAL_ID__$/    id-1 = $remote_id/" \
  "$CONFIG" > "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
chmod 600 "$SWANCTL_WORK_BASE/site-a/swanctl.conf" "$SWANCTL_WORK_BASE/site-b/swanctl.conf"

if [ "${GRE_DYNAMIC_TS:-0}" = "1" ]; then
  sed -i \
    -e 's/^        local_ts = .*/        local_ts = dynamic[gre]/' \
    -e 's/^        remote_ts = .*/        remote_ts = dynamic[gre]/' \
    "$SWANCTL_WORK_BASE/site-a/swanctl.conf" "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
  printf 'Using dynamic GRE transport selectors for VPP-owned outer addresses.\n'
fi

start_node site-a
start_node site-b
swanctl --load-conns --uri "unix://$RUN_BASE/site-a/charon.vici" --file "$SWANCTL_WORK_BASE/site-a/swanctl.conf"
swanctl --load-creds --uri "unix://$RUN_BASE/site-a/charon.vici" --file "$SWANCTL_WORK_BASE/site-a/swanctl.conf"
swanctl --load-conns --uri "unix://$RUN_BASE/site-b/charon.vici" --file "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
swanctl --load-creds --uri "unix://$RUN_BASE/site-b/charon.vici" --file "$SWANCTL_WORK_BASE/site-b/swanctl.conf"
swanctl --list-conns --uri "unix://$RUN_BASE/site-a/charon.vici" >&2
swanctl --list-conns --uri "unix://$RUN_BASE/site-b/charon.vici" >&2
swanctl --initiate --uri "unix://$RUN_BASE/site-a/charon.vici" --child "$child_id"

printf 'GRE over IPsec CHILD_SA established.\n'
)

ipsec_gre_stop() (
if [ "$(id -u)" != "0" ]; then
  printf 'Please run as root: sudo %s\n' "$0" >&2
  exit 1
fi

RUN_BASE="${GRE_RUN_BASE:-/run/eventnet-netns-ipsec-gre}"
for ns in site-a site-b; do
  run_dir="$RUN_BASE/$ns"
  if [ -s "$run_dir/charon.pid" ]; then kill "$(cat "$run_dir/charon.pid")" 2>/dev/null || true; fi
  if [ -s "$run_dir/eventnet-wrapper.pid" ]; then kill "$(cat "$run_dir/eventnet-wrapper.pid")" 2>/dev/null || true; fi
  for namespace_pid in $(ip netns pids "$ns" 2>/dev/null || true); do
    if [ -r "/proc/$namespace_pid/comm" ] && [ "$(cat "/proc/$namespace_pid/comm")" = "charon" ]; then
      kill "$namespace_pid" 2>/dev/null || true
    fi
  done
  ip netns exec "$ns" ip xfrm state flush 2>/dev/null || true
  ip netns exec "$ns" ip xfrm policy flush 2>/dev/null || true
  rm -f "$run_dir/charon.pid" "$run_dir/eventnet-wrapper.pid" "$run_dir/charon.vici"
done
printf 'Stopped GRE charon processes and flushed GRE XFRM state.\n'
)

case "$ACTION" in
  generate|start|smoke|stop)
    run_existing "$ACTION" "$@"
    ;;
  clean)
    if [ "$MODE" = "direct" ]; then
      clean_direct
    elif [ "$MODE" = "hub" ]; then
      clean_hub
    else
      clean_gre
    fi
    ;;
  logs)
    show_logs
    ;;
  status)
    show_status
    ;;
  *)
    usage
    ;;
esac
