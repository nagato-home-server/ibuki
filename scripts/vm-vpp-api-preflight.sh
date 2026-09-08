#!/usr/bin/env sh
set -eu

VPP_PREFIX="${VPP_PREFIX:-}"
printf '%s\n' 'VPP Binary API preflight'
if [ "$(uname -s)" != "Linux" ]; then
  printf '%s\n' 'skip: VPP Binary API preflight is Linux-oriented'
  exit 0
fi

missing=0
for path in \
  "$VPP_PREFIX/include/vapi/vapi.h" "$VPP_PREFIX/include/vpp/vapi/vapi.h" \
  "$VPP_PREFIX/include/vpp-api/vapi/vapi.h" "$VPP_PREFIX/include/vpp_plugins/vpp-api/client/vapi/vapi.h" \
  "$VPP_PREFIX/include/vpp_plugins/vpp-api/client/vapi.h" \
  /usr/include/vapi/vapi.h /usr/local/include/vapi/vapi.h \
  /usr/include/vpp/vapi/vapi.h /usr/local/include/vpp/vapi/vapi.h \
  /usr/include/vpp-api/vapi/vapi.h /usr/local/include/vpp-api/vapi/vapi.h \
  /usr/include/vpp_plugins/vpp-api/client/vapi/vapi.h /usr/local/include/vpp_plugins/vpp-api/client/vapi/vapi.h \
  /usr/include/vpp_plugins/vpp-api/client/vapi.h /usr/local/include/vpp_plugins/vpp-api/client/vapi.h; do
  if [ -f "$path" ]; then printf 'ok: header -> %s\n' "$path"; header_found=1; break; fi
done
: "${header_found:=0}"
if [ "$header_found" -eq 0 ]; then printf '%s\n' 'missing: vapi/vapi.h'; missing=1; fi

library_found=0
for path in "$VPP_PREFIX/lib/libvapi.so" "$VPP_PREFIX/lib/libvapiclient.so" "$VPP_PREFIX/lib/libvppapiclient.so" \
  /lib*/libvapi.so /usr/lib*/libvapi.so /usr/local/lib*/libvapi.so \
  /lib*/libvapiclient.so /usr/lib*/libvapiclient.so /usr/local/lib*/libvapiclient.so \
  /lib*/libvppapiclient.so /usr/lib*/libvppapiclient.so /usr/local/lib*/libvppapiclient.so; do
  if [ -e "$path" ]; then printf 'ok: library -> %s\n' "$path"; library_found=1; break; fi
done
if [ "$library_found" -eq 0 ]; then printf '%s\n' 'missing: libvapi.so, libvapiclient.so, or libvppapiclient.so'; missing=1; fi

if [ "$missing" -ne 0 ]; then
  printf '%s\n' 'VPP Binary API is unavailable; keep using the vppctl adapter.'
  exit 1
fi
printf '%s\n' 'VPP Binary API dependency check passed.'
