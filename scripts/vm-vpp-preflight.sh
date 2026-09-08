#!/usr/bin/env sh
set -eu

missing=0

check_cmd() {
  name="$1"
  if command -v "$name" >/dev/null 2>&1; then
    printf 'ok: %s -> %s\n' "$name" "$(command -v "$name")"
  else
    printf 'missing: %s\n' "$name"
    missing=1
  fi
}

check_service() {
  service="$1"
  if command -v systemctl >/dev/null 2>&1; then
    if systemctl is-active "$service" >/dev/null 2>&1; then
      printf 'active: %s\n' "$service"
    else
      printf 'not-active: %s\n' "$service"
    fi
  fi
}

check_cmd vpp
check_cmd vppctl
check_cmd ip
check_service vpp

if [ "${VPP_TOPOLOGY:-edge}" = "hub" ]; then
  printf '\nHub topology prerequisites:\n'
  if ip netns list | awk '{print $1}' | grep -qx 'hub-1'; then
    printf 'ok: namespace hub-1\n'
  else
    printf 'missing: namespace hub-1\n'
    missing=1
  fi
  if command -v vpp >/dev/null 2>&1 && vpp --help 2>&1 | grep -q -- '-c'; then
    printf 'ok: VPP supports per-instance config option\n'
  else
    printf 'unknown: VPP per-instance config option\n'
  fi
  printf 'note: this preflight does not start a second VPP instance.\n'
fi

if command -v vppctl >/dev/null 2>&1; then
  printf '\nVPP version:\n'
  vppctl show version 2>/dev/null || true
  printf '\nVPP interfaces:\n'
  vppctl show interface 2>/dev/null || true
  printf '\nVPP IPv4 FIB:\n'
  vppctl show ip fib 2>/dev/null || true
fi

if [ "$missing" -ne 0 ]; then
  cat <<'MSG'

VPP is not fully available yet. Dry-run route plan generation still works.

Debian/Ubuntu package names vary by release:
  sudo apt-get install -y vpp vpp-plugin-core

If distro packages are unavailable, use FD.io VPP packages for your OS release.
MSG
  exit 1
fi
