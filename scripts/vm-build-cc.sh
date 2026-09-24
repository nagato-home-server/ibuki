#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c17 -Wall -Wextra -Wpedantic -O2 -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_POSIX_C_SOURCE=200809L}"
LDLIBS="${LDLIBS:--lm}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"

mkdir -p "$BUILD_DIR"

COMMON_SRCS="
third_party/yyjson/yyjson.c
src/json_output.c
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

COMMON_OBJECTS=""
object_index=0
for source in $COMMON_SRCS; do
    object="$BUILD_DIR/common-${object_index}.o"
    if [ "$FORCE_REBUILD" = "1" ] || [ ! -f "$object" ] || [ "$ROOT_DIR/$source" -nt "$object" ]; then
        printf '[build] compiling common object %s\n' "$source"
        $CC $CFLAGS -Iinclude -Ithird_party/yyjson -c "$source" -o "$object"
    else
        printf '[build] reusing common object %s\n' "$source"
    fi
    COMMON_OBJECTS="$COMMON_OBJECTS $object"
    object_index=$((object_index + 1))
done

compile_target() {
    target=$1
    source=$2
    printf '[build] linking %s\n' "$target"
    $CC $CFLAGS -Iinclude -Ithird_party/yyjson "$source" $COMMON_OBJECTS -o "$BUILD_DIR/$target" $LDLIBS
    printf '[build] completed %s\n' "$target"
}

compile_target eventnet_yaml_demo examples/yaml_demo.c
compile_target eventnet_netns_plan examples/netns_plan.c
compile_target eventnet_scenario examples/eventnet_scenario.c
compile_target eventnet_demo examples/demo.c
compile_target eventnet_agent examples/eventnet_agent.c
compile_target eventnetd examples/eventnetd.c
compile_target eventnet_swanctl_observer examples/swanctl_observer.c
compile_target eventnet_vpp_observer examples/vpp_observer.c
compile_target eventnet_vpp_interface_observer examples/vpp_interface_observer.c
compile_target eventnet_tests tests/test_controller.c

printf '[build] running eventnet_tests\n'
"$BUILD_DIR/eventnet_tests"
printf '[build] eventnet_tests passed\n'

printf '\nBuilt without CMake: %s\n' "$BUILD_DIR"
