#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux}"
VPP_API="${EVENTNET_ENABLE_VPP_API:-OFF}"
STRONGSWAN_VICI="${EVENTNET_ENABLE_STRONGSWAN_VICI:-OFF}"
HARDENING="${EVENTNET_ENABLE_HARDENING:-ON}"
VPP_PREFIX="${VPP_PREFIX:-}"

cd "$ROOT_DIR"
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cached_root=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n 1)
  if [ -n "$cached_root" ] && [ "$cached_root" != "$ROOT_DIR" ]; then
    printf 'stale CMake cache: %s points to %s (current: %s)\n' "$BUILD_DIR" "$cached_root" "$ROOT_DIR" >&2
    printf '%s\n' 'choose a new BUILD_DIR or remove the old generated build directory' >&2
    exit 2
  fi
fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$VPP_PREFIX" \
  -DEVENTNET_ENABLE_VPP_API="$VPP_API" \
  -DEVENTNET_ENABLE_STRONGSWAN_VICI="$STRONGSWAN_VICI" \
  -DEVENTNET_ENABLE_HARDENING="$HARDENING"
cmake --build "$BUILD_DIR"
ctest --test-dir "$BUILD_DIR" --output-on-failure

printf '\nBuilt: %s\n' "$BUILD_DIR"
