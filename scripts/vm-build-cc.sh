#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -Wall -Wextra -Wpedantic -O2 -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L}"
LDLIBS="${LDLIBS:--lm}"

mkdir -p "$BUILD_DIR"

COMMON_SRCS="
src/apply_plan.c
src/audit.c
src/command_adapters.c
src/controller.c
src/health_probe.c
src/path_selection.c
src/render_commands.c
src/state.c
src/strongswan_adapter_mock.c
src/topology.c
src/transition.c
src/telemetry.c
src/strongswan_observer.c
src/strongswan_vici_adapter.c
src/vpp_observer.c
src/vpp_api_adapter.c
src/vpp_api_transport.c
src/vpp_adapter_mock.c
src/yaml_config.c
"

cd "$ROOT_DIR"

$CC $CFLAGS -Iinclude $COMMON_SRCS examples/yaml_demo.c -o "$BUILD_DIR/eventnet_yaml_demo" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/netns_plan.c -o "$BUILD_DIR/eventnet_netns_plan" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/eventnet_scenario.c -o "$BUILD_DIR/eventnet_scenario" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/demo.c -o "$BUILD_DIR/eventnet_demo" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/eventnet_agent.c -o "$BUILD_DIR/eventnet_agent" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/eventnetd.c -o "$BUILD_DIR/eventnetd" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/swanctl_observer.c -o "$BUILD_DIR/eventnet_swanctl_observer" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/vpp_observer.c -o "$BUILD_DIR/eventnet_vpp_observer" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS examples/vpp_interface_observer.c -o "$BUILD_DIR/eventnet_vpp_interface_observer" $LDLIBS
$CC $CFLAGS -Iinclude $COMMON_SRCS tests/test_controller.c -o "$BUILD_DIR/eventnet_tests" $LDLIBS

"$BUILD_DIR/eventnet_tests"

printf '\nBuilt without CMake: %s\n' "$BUILD_DIR"
