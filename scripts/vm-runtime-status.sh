#!/usr/bin/env sh
set -eu

check_cmd() {
  if command -v "$1" >/dev/null 2>&1; then
    printf 'ok: %s -> %s\n' "$1" "$(command -v "$1")"
  else
    printf 'missing: %s\n' "$1"
  fi
}

find_file() {
  label="$1"
  shift
  for path in "$@"; do
    if [ -x "$path" ]; then
      printf 'ok: %s -> %s\n' "$label" "$path"
      return 0
    fi
  done
  printf 'missing: %s\n' "$label"
  return 1
}

check_cmd_or_file() {
  label="$1"
  shift
  if command -v "$label" >/dev/null 2>&1; then
    printf 'ok: %s -> %s\n' "$label" "$(command -v "$label")"
    return 0
  fi
  find_file "$label" "$@"
}

check_service() {
  service="$1"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl is-active "$service" >/dev/null 2>&1 && \
      printf 'active: %s\n' "$service" || \
      printf 'not-active: %s\n' "$service"
  fi
}

check_cmd swanctl
check_cmd_or_file charon /usr/lib/ipsec/charon /usr/lib/strongswan/charon /usr/libexec/ipsec/charon || true
find_file starter /usr/lib/ipsec/starter /usr/lib/strongswan/starter /usr/libexec/ipsec/starter || true
check_cmd vppctl
check_cmd vpp
check_cmd ip
check_cmd tcpdump
check_cmd unshare
check_cmd mount

check_service strongswan
check_service strongswan-starter
check_service vpp

printf '\nNamespaces:\n'
ip netns list || true

printf '\nGenerated plan files:\n'
ls -l out/eventnet-swanctl.conf out/apply-eventnet.sh 2>/dev/null || true
