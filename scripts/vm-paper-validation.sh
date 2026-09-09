#!/usr/bin/env sh
set -u

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT_DIR/build-paper-baseline}
REPORT_DIR=${OUT_DIR:-$ROOT_DIR/out/paper-validation/$(date +%Y%m%d-%H%M%S)}
CHILD_OUT_DIR=${CHILD_OUT_DIR:-${TMPDIR:-/tmp}/ibuki-paper-validation-$(date +%Y%m%d-%H%M%S)-$$}
YAML=${1:-samples/linux-vm-netns.yaml}
RUN_RUNTIME=${RUN_RUNTIME:-0}

mkdir -p "$REPORT_DIR" "$CHILD_OUT_DIR"
cd "$ROOT_DIR"
SUMMARY="$REPORT_DIR/summary.csv"
printf '%s\n' 'case,status,log' > "$SUMMARY"
export BUILD_DIR
export OUT_DIR="$CHILD_OUT_DIR"
overall=0

run_case() {
    name=$1
    shift
    log="$REPORT_DIR/$name.log"
    printf 'validation: %s\n' "$name"
    if "$@" >"$log" 2>&1; then
        printf '%s,pass,%s\n' "$name" "$log" >> "$SUMMARY"
    else
        status=$?
        printf '%s,fail,%s\n' "$name" "$log" >> "$SUMMARY"
        printf 'validation failed: %s (status %s)\n' "$name" "$status" >&2
        if [ -s "$log" ]; then
            printf '%s\n' "--- $name log (tail) ---" >&2
            tail -n 40 "$log" >&2 || true
        fi
        overall=1
    fi
}

run_root_case() {
    name=$1
    shift
    if [ "$(id -u)" = "0" ]; then
        run_case "$name" "$@"
    else
        printf '%s,skip,%s\n' "$name" 'root required; rerun with sudo' >> "$SUMMARY"
        printf 'validation: %s -> skip (root required)\n' "$name"
    fi
}

run_vpp_root_case() {
    name=$1
    shift
    if [ "$(id -u)" != "0" ]; then
        printf '%s,skip,%s\n' "$name" 'root required; rerun with sudo' >> "$SUMMARY"
        printf 'validation: %s -> skip (root required)\n' "$name"
    elif ! command -v vppctl >/dev/null 2>&1; then
        printf '%s,skip,%s\n' "$name" 'vppctl unavailable; install VPP to run' >> "$SUMMARY"
        printf 'validation: %s -> skip (vppctl unavailable)\n' "$name"
    else
        run_case "$name" "$@"
    fi
}

if [ ! -x "$BUILD_DIR/eventnet_tests" ]; then
    run_case build "sh" scripts/vm-build-cc.sh
fi

run_case c-unit "$BUILD_DIR/eventnet_tests"
run_case scenario "sh" scripts/vm-eventnet-scenario-smoke.sh "$YAML"
run_case route-yaml "sh" scripts/vm-route-yaml-smoke.sh samples/route-examples.yaml
run_case agent "sh" scripts/vm-agent-smoke.sh "$YAML"
run_case telemetry-controller "env" DEBUG_VALIDATION=1 sh scripts/vm-telemetry-controller-smoke.sh
run_case threshold "sh" scripts/vm-threshold-smoke.sh samples/route-examples.yaml
run_case stability "sh" scripts/vm-stability-smoke.sh "$YAML"
run_case event-reconcile "env" DEBUG_VALIDATION=1 sh scripts/vm-eventnet-event-smoke.sh "$YAML"
run_case reload-sighup "sh" scripts/vm-eventnet-sighup-smoke.sh "$YAML"
run_vpp_root_case vlan-policy "sh" scripts/vm-vpp-vlan-netns-smoke.sh
run_vpp_root_case vpp-routes "sh" scripts/vm-vpp-routes-yaml-netns-smoke.sh samples/vpp-netns-routes.yaml
cp samples/telemetry-replay.jsonl "$CHILD_OUT_DIR/telemetry-replay.jsonl"
export TELEMETRY="$CHILD_OUT_DIR/telemetry-replay.jsonl"
run_case status-security "sh" scripts/vm-eventnetd-status-security-smoke.sh "$YAML"
run_case plan-security "sh" scripts/vm-plan-secret-permission-smoke.sh "$YAML"
run_case shell-syntax "sh" scripts/vm-shell-check.sh

if [ "$RUN_RUNTIME" = "1" ]; then
    RUNTIME_OUT_DIR="$ROOT_DIR/out/paper-validation-runtime-$(date +%Y%m%d-%H%M%S)-$$"
    mkdir -p "$RUNTIME_OUT_DIR"
    export OUT_DIR="$RUNTIME_OUT_DIR"
    prepare_runtime_case() {
        sh scripts/vm-netns-ipsec-direct-stop.sh >/dev/null 2>&1 || true
        sh scripts/vm-netns-ipsec-hub-stop.sh >/dev/null 2>&1 || true
    }
    prepare_runtime_case
    run_root_case xfrm-policy-runtime "sh" scripts/vm-xfrm-policy-runtime-smoke.sh "$YAML"
    prepare_runtime_case
    run_root_case xfrm-cleartext "sh" scripts/vm-xfrm-cleartext-smoke.sh "$YAML"
    prepare_runtime_case
    run_root_case runtime-rollback "sh" scripts/vm-runtime-rollback-smoke.sh
    prepare_runtime_case
    run_root_case integrated-direct "sh" scripts/vm-controller-integrated-runtime-smoke.sh "$YAML"
    prepare_runtime_case
    run_root_case integrated-fallback "env" MODE=fallback sh scripts/vm-controller-integrated-runtime-smoke.sh "$YAML"
    export OUT_DIR="$CHILD_OUT_DIR"
else
    printf '%s,skip,%s\n' runtime 'set RUN_RUNTIME=1 and run as root' >> "$SUMMARY"
fi

printf '\nValidation summary: %s\n' "$SUMMARY"
cat "$SUMMARY"
exit "$overall"
