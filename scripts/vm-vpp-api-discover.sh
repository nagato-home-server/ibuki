#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_FILE=${OUT_FILE:-$ROOT_DIR/out/vpp-api-discovery.txt}
VPP_PREFIX=${VPP_PREFIX:-}

if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
  printf '%s\n' 'VPP API discovery is Linux-oriented.' >&2
  exit 2
fi

mkdir -p "$(dirname "$OUT_FILE")"
{
  printf '%s\n' '== VPP API discovery =='
  printf 'timestamp: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'kernel: '; uname -a
  if command -v vpp >/dev/null 2>&1; then vpp -version 2>&1 || true; else printf '%s\n' 'vpp: missing'; fi
  if command -v vppctl >/dev/null 2>&1; then vppctl show version 2>&1 || true; else printf '%s\n' 'vppctl: missing'; fi
  printf 'VPP_PREFIX: %s\n' "${VPP_PREFIX:-<unset>}"
  printf '%s\n' '== package and linker metadata =='
  if command -v dpkg-query >/dev/null 2>&1; then
    dpkg-query -W -f='${Package} ${Version}\n' 'vpp*' 2>/dev/null || true
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    for package in vpp vapi vapiclient vppapiclient; do
      if pkg-config --exists "$package"; then
        printf 'pkg-config %s: ' "$package"
        pkg-config --modversion --cflags --libs "$package" 2>/dev/null || true
      fi
    done
  fi
  if command -v ldconfig >/dev/null 2>&1; then
    ldconfig -p 2>/dev/null | grep -E 'lib(vapi|vapiclient|vppapiclient)\.so' || true
  fi
  printf '%s\n' '== headers =='
  for root in /usr/include /usr/local/include "${VPP_PREFIX:-/nonexistent}/include"; do
    [ -d "$root" ] || continue
    find "$root" -type f \( -name 'vapi.h' -o -name 'vpe_all_api_h.h' -o -name 'interfaces_all_api_h.h' -o -name '*api_h.h' \) -print 2>/dev/null
  done | sort -u
  printf '%s\n' '== API JSON =='
  for root in /usr/share/vpp/api /usr/local/share/vpp/api "${VPP_PREFIX:-/nonexistent}/share/vpp/api"; do
    [ -d "$root" ] || continue
    find "$root" -type f -name '*.json' -print 2>/dev/null
  done | sort -u
  printf '%s\n' '== relevant symbols =='
  for root in /usr/include /usr/local/include "${VPP_PREFIX:-/nonexistent}/include" /usr/share/vpp/api /usr/local/share/vpp/api "${VPP_PREFIX:-/nonexistent}/share/vpp/api"; do
    [ -d "$root" ] || continue
    if command -v rg >/dev/null 2>&1; then
      rg -n -H 'ip_route_add_del|ip_table_add_del|create_vlan_subif|sw_interface_set_flags|sw_interface_set_l2' "$root" 2>/dev/null || true
    else
      grep -RInE 'ip_route_add_del|ip_table_add_del|create_vlan_subif|sw_interface_set_flags|sw_interface_set_l2' "$root" 2>/dev/null || true
    fi
  done
} | tee "$OUT_FILE"

printf 'VPP API discovery written: %s\n' "$OUT_FILE"
