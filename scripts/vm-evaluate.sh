#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CASE="${1:-all}"
YAML="${2:-samples/linux-vm-netns.yaml}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-linux-cc}"
EVAL_REPEAT="${EVAL_REPEAT:-5}"
RUN_RUNTIME="${RUN_RUNTIME:-0}"
INTENT_ID="${INTENT_ID:-intent-a-b}"

timestamp=$(date +%Y%m%d-%H%M%S)
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/evaluation/$timestamp}"
SUMMARY_CSV="$OUT_DIR/summary.csv"
SUMMARY_MD="$OUT_DIR/summary.md"

usage() {
  cat <<EOF
Usage:
  sh scripts/vm-evaluate.sh [case] [yaml]

Cases:
  all                 Run normal-user checks and write an evaluation report.
  scenario            Check direct/fallback/evaluated/multi-step selection.
  telemetry           Check Agent telemetry and eventnetd integration.
  telemetry-long      Stream periodic Agent telemetry into eventnetd.
  telemetry-live      Stream live Agent records while eventnetd reloads telemetry periodically.
  threshold           Check Agent streak thresholds and anti-flap recovery.
  stability           Check evaluated hysteresis with multi-round telemetry.
  xfrm-policy         Check scoped IPsec cleartext-block plan generation.
  xfrm-runtime        Check scoped XFRM block policy installation in netns. Root required.
  xfrm-cleartext      Check encrypted traffic and post-SA cleartext blocking. Root required.
  fallback-time       Measure fallback plan generation time.
  explain             Check explain JSONL can replay selected path decisions.
  integrated-direct   Run integrated IPsec + VPP direct runtime smoke. Root required.
  integrated-fallback Run integrated IPsec + VPP fallback runtime smoke. Root required.
  transition-policy   Summarize Immediate/Graceful/Flow Preserve coverage.
  rollback            Summarize rollback coverage and current measurable gap.
  rollback-runtime    Inject an apply failure and verify runtime cleanup. Root required.
  control-plane       Summarize control-plane failure coverage.
  restart-recovery    Summarize restart/observed-state recovery coverage.
  state-boundary      Check state-file Intent and Path scope validation.
  vlan-policy         Check VLAN Policy metadata and VPP sub-interface plan.
  node-capability     Check YAML Node capabilities and capability-constrained selection.
  route-yaml          Check all supported YAML route forms and invalid input.
  reload-sighup       Check Linux SIGHUP reload and invalid-config retention.
  event-reconcile     Check path failure/recovery event input and socket stream.
  swanctl-observer    Check swanctl CHILD SA output to Observed State conversion.
  vici-runtime        Check optional libvici version, CHILD observe, and event monitor.
  vici-controller     Check libvici-driven controller reconcile.
  vici-eventnetd-internal Check eventnetd internal VICI subscription and stop.
  vpp-observer        Check VPP FIB output to route Observed State conversion.
  vpp-api             Check VPP Binary API dependency availability and scope.
  observer-runtime    Feed real strongSwan/VPP observer events into eventnetd. Root required.
  cert-auth           Check pubkey certificate files and validity window.
  plan-secret-permission Check generated swanctl.conf permissions and symlink protection.
  status-output-security Check eventnetd status JSONL output protection.
  service-unit        Check the systemd eventnetd unit safety contract.
  backend-reuse       Check core selector can feed IPsec/VPP plan generators.
  federation          Summarize multi-domain proposal coverage.

Environment:
  OUT_DIR=path        Report output directory. Default: out/evaluation/YYYYMMDD-HHMMSS
  EVAL_REPEAT=n      fallback-time iterations. Default: 5
  RUN_RUNTIME=1      In 'all', also run root runtime checks when executed as root.
  VICI_BUILD_DIR=dir Optional CMake build containing eventnet_strongswan_vici_probe.
  VICI_URI=uri VICI_CHILD=id VICI_PATH=id  Required for vici-runtime.
  RUN_INTERNAL=1      Use eventnetd internal VICI monitor in its smoke script.
EOF
}

now_ns() {
  value=$(date +%s%N 2>/dev/null || true)
  case "$value" in
    *N*|"") printf '%s000000000\n' "$(date +%s)" ;;
    *) printf '%s\n' "$value" ;;
  esac
}

elapsed_ms() {
  start_ns="$1"
  end_ns=$(now_ns)
  printf '%s\n' "$(( (end_ns - start_ns) / 1000000 ))"
}

csv_escape() {
  printf '%s' "$1" | tr '\n,' '  '
}

file_sha256() {
  hash_target="$1"
  if [ ! -f "$hash_target" ]; then
    printf '%s\n' 'missing'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$hash_target" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$hash_target" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$hash_target" | awk '{print $NF}'
  else
    printf '%s\n' 'unavailable'
  fi
}

worktree_diff_sha256() {
  if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
    printf '%s\n' 'unavailable'
  elif command -v sha256sum >/dev/null 2>&1; then
    git diff --binary HEAD 2>/dev/null | sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    git diff --binary HEAD 2>/dev/null | shasum -a 256 | awk '{print $1}'
  else
    printf '%s\n' 'unavailable'
  fi
}

record_case() {
  name="$1"
  status="$2"
  duration_ms="$3"
  details="$4"
  printf '%s,%s,%s,%s\n' "$(csv_escape "$name")" "$(csv_escape "$status")" "$(csv_escape "$duration_ms")" "$(csv_escape "$details")" >> "$SUMMARY_CSV"
  printf '| `%s` | `%s` | `%s` | %s |\n' "$name" "$status" "$duration_ms" "$details" >> "$SUMMARY_MD"
  printf 'eval: %s -> %s (%sms)\n' "$name" "$status" "$duration_ms" >&2
}

start_report() {
  mkdir -p "$OUT_DIR"
  {
    printf 'case=%s\n' "$CASE"
    printf 'yaml=%s\n' "$YAML"
    printf 'timestamp=%s\n' "$timestamp"
    printf 'build_dir=%s\n' "$BUILD_DIR"
    printf 'eval_repeat=%s\n' "$EVAL_REPEAT"
    printf 'run_runtime=%s\n' "$RUN_RUNTIME"
    printf 'yaml_sha256=%s\n' "$(file_sha256 "$YAML")"
    printf 'git_revision='
    git rev-parse --verify HEAD 2>/dev/null || printf 'unknown'
    printf '\n'
    if git diff --quiet && git diff --cached --quiet; then
      printf 'git_worktree=clean\n'
    else
      printf 'git_worktree=modified\n'
    fi
    printf 'git_worktree_diff_sha256=%s\n' "$(worktree_diff_sha256)"
    os_info=$(uname -srm 2>/dev/null || true)
    printf 'os=%s\n' "${os_info:-unknown}"
    shell_info=$(sh --version 2>/dev/null | head -n 1 || true)
    printf 'shell=%s\n' "${shell_info:-unknown}"
    if [ -x "$BUILD_DIR/eventnetd" ]; then
      eventnetd_info=$("$BUILD_DIR/eventnetd" --help 2>/dev/null | head -n 1 || true)
    else
      eventnetd_info=missing
    fi
    printf 'eventnetd=%s\n' "${eventnetd_info:-unknown}"
    if [ -x "$BUILD_DIR/eventnet_agent" ]; then
      agent_info=$("$BUILD_DIR/eventnet_agent" --help 2>/dev/null | head -n 1 || true)
    else
      agent_info=missing
    fi
    printf 'eventnet_agent=%s\n' "${agent_info:-unknown}"
    printf 'eventnetd_sha256=%s\n' "$(file_sha256 "$BUILD_DIR/eventnetd")"
    printf 'eventnet_agent_sha256=%s\n' "$(file_sha256 "$BUILD_DIR/eventnet_agent")"
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
      sed -n '/^EVENTNET_ENABLE_VPP_API:/p; /^EVENTNET_ENABLE_STRONGSWAN_VICI:/p' "$BUILD_DIR/CMakeCache.txt"
    else
      printf '%s\n' 'cmake_features=unavailable'
    fi
  } > "$OUT_DIR/environment.txt"
  printf 'Ibuki evaluation started: case=%s yaml=%s\n' "$CASE" "$YAML" >&2
  printf 'Evaluation output directory: %s\n' "$OUT_DIR" >&2
  printf 'Evaluation environment: %s\n' "$OUT_DIR/environment.txt" >&2
  printf 'case,status,duration_ms,details\n' > "$SUMMARY_CSV"
  {
    printf '# Ibuki Evaluation Report\n\n'
    printf '%s\n' "- yaml: \`$YAML\`"
    printf '%s\n' "- out_dir: \`$OUT_DIR\`"
    printf '%s\n\n' "- generated_at: \`$(date)\`"
    printf '| case | status | duration_ms | details |\n'
    printf '| --- | --- | ---: | --- |\n'
  } > "$SUMMARY_MD"
}

finish_report() {
  status="$1"
  if [ -f "$SUMMARY_MD" ] && ! grep -q '^## Totals$' "$SUMMARY_MD"; then
    pass_count=$(awk -F, 'NR > 1 && $2 == "pass" { count++ } END { print count + 0 }' "$SUMMARY_CSV")
    partial_count=$(awk -F, 'NR > 1 && $2 == "partial" { count++ } END { print count + 0 }' "$SUMMARY_CSV")
    skip_count=$(awk -F, 'NR > 1 && $2 == "skip" { count++ } END { print count + 0 }' "$SUMMARY_CSV")
    fail_count=$(awk -F, 'NR > 1 && $2 == "fail" { count++ } END { print count + 0 }' "$SUMMARY_CSV")
    {
      printf '\n## Totals\n\n'
      printf -- '- pass: `%s`\n' "$pass_count"
      printf -- '- partial: `%s`\n' "$partial_count"
      printf -- '- skip: `%s`\n' "$skip_count"
      printf -- '- fail: `%s`\n' "$fail_count"
    } >> "$SUMMARY_MD"
  fi
  printf '\nEvaluation report written:\n'
  printf '  %s\n' "$SUMMARY_MD"
  printf '  %s\n' "$SUMMARY_CSV"
  if [ "$status" != "0" ]; then
    printf 'Evaluation completed with failures. See logs under: %s\n' "$OUT_DIR" >&2
  fi
}

on_exit() {
  status="$?"
  if [ -n "${SUMMARY_MD:-}" ] && [ -f "$SUMMARY_MD" ]; then
    finish_report "$status"
  fi
}

trap on_exit 0

ensure_build() {
  if [ ! -x "$BUILD_DIR/eventnet_scenario" ] || [ ! -x "$BUILD_DIR/eventnet_netns_plan" ] || [ ! -x "$BUILD_DIR/eventnet_yaml_demo" ] || [ ! -x "$BUILD_DIR/eventnet_swanctl_observer" ] || [ ! -x "$BUILD_DIR/eventnet_vpp_observer" ] || [ ! -x "$BUILD_DIR/eventnet_vpp_interface_observer" ] || [ ! -x "$BUILD_DIR/eventnet_agent" ] || [ ! -x "$BUILD_DIR/eventnetd" ] || [ ! -x "$BUILD_DIR/eventnet_tests" ]; then
    sh "$ROOT_DIR/scripts/vm-build-cc.sh" > "$OUT_DIR/build.log" 2>&1
  fi
}

case_scenario() {
  name="scenario"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if (OUT_DIR="$OUT_DIR/scenario" sh "$ROOT_DIR/scripts/vm-eventnet-scenario-smoke.sh" "$YAML") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "direct/fallback/evaluated/multi-step selection passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "scenario smoke failed; log: $log"
    return 1
  fi
}

case_telemetry() {
  name="telemetry"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if sh "$ROOT_DIR/scripts/vm-telemetry-controller-smoke.sh" > "$log" 2>&1 &&
    (BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR/agent" sh "$ROOT_DIR/scripts/vm-agent-smoke.sh" "$YAML") >> "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "Agent telemetry, batching, freshness, stdin, socket, and status integration passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "Agent/eventnetd telemetry integration failed; log: $log"
    return 1
  fi
}

case_telemetry_long() {
  name="telemetry-long"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  count="${LONG_COUNT:-5}"
  interval_ms="${LONG_INTERVAL_MS:-1000}"
  status_jsonl="$OUT_DIR/$name-status.jsonl"
  state_file="$OUT_DIR/$name.state"
  agent_jsonl="$OUT_DIR/$name-agent.jsonl"
  if ! printf '%s\n' "$count" | awk '($1 ~ /^[1-9][0-9]*$/) { exit 0 } { exit 1 }' ||
    ! printf '%s\n' "$interval_ms" | awk '($1 ~ /^[0-9]+$/) { exit 0 } { exit 1 }'; then
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "LONG_COUNT must be positive and LONG_INTERVAL_MS must be non-negative; log: $log"
    return 1
  fi
  if "$BUILD_DIR/eventnet_agent" --yaml "$YAML" --intent "$INTENT_ID" \
      --count "$count" --interval-ms 0 --simulate 12 0 \
      --output "$agent_jsonl" 2> "$OUT_DIR/$name-agent.log" &&
    agent_batch_size=$(wc -l < "$agent_jsonl" | tr -d ' ') &&
    [ "$agent_batch_size" -gt 0 ] &&
    "$BUILD_DIR/eventnetd" "$YAML" --intent "$INTENT_ID" --telemetry "$agent_jsonl" \
      --batch-size "$agent_batch_size" --count "$count" --interval-ms "$interval_ms" --state-file "$state_file" \
      --status-jsonl "$status_jsonl" > "$log" 2>&1; then
    status_count=$(wc -l < "$status_jsonl" | tr -d ' ')
    duration_ms=$(elapsed_ms "$start_ns")
    minimum_duration_ms=$(( (count - 1) * interval_ms ))
    if [ "$status_count" -eq "$count" ] && grep -q "eventnetd_iteration: $count/" "$log" && [ "$duration_ms" -ge "$minimum_duration_ms" ]; then
      record_case "$name" "pass" "$duration_ms" "Agent batch and eventnetd periodic reconcile produced $count iterations; eventnetd_interval_ms=$interval_ms; minimum_duration_ms=$minimum_duration_ms; log: $log"
    else
      record_case "$name" "fail" "$duration_ms" "periodic count or duration mismatch: expected $count status records, got $status_count; duration_ms=$duration_ms minimum_duration_ms=$minimum_duration_ms; log: $log"
      return 1
    fi
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "periodic Agent/eventnetd stream failed; log: $log"
    return 1
  fi
}

case_telemetry_live() {
  name="telemetry-live"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if (BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR/$name" LIVE_COUNT="${LIVE_COUNT:-4}" LIVE_INTERVAL_MS="${LIVE_INTERVAL_MS:-250}" \
    sh "$ROOT_DIR/scripts/vm-agent-eventnetd-live-smoke.sh" "$YAML") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "live Agent records were appended while eventnetd periodically reloaded and reconciled them; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "live Agent/eventnetd periodic smoke failed; log: $log"
    return 1
  fi
}

case_threshold() {
  name="threshold"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  threshold_yaml="${THRESHOLD_YAML:-$ROOT_DIR/samples/route-examples.yaml}"
  if (BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR/$name" sh "$ROOT_DIR/scripts/vm-threshold-smoke.sh" "$threshold_yaml") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "Agent consecutive telemetry controlled direct maintenance, fallback, and recovery; yaml: $threshold_yaml; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "threshold smoke failed; yaml: $threshold_yaml; log: $log"
    return 1
  fi
}

case_stability() {
  name="stability"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if OUT_DIR="$OUT_DIR/$name" sh "$ROOT_DIR/scripts/vm-stability-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "evaluated hysteresis retained active path before sufficient improvement; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "evaluated hysteresis smoke failed; log: $log"
    return 1
  fi
}

case_xfrm_policy() {
  name="xfrm-policy"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  xfrm_yaml="${XFRM_YAML:-$ROOT_DIR/samples/route-examples.yaml}"
  if (BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR/$name" sh "$ROOT_DIR/scripts/vm-xfrm-policy-smoke.sh" "$xfrm_yaml") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "scoped bidirectional XFRM block and default-off plan generation passed; yaml: $xfrm_yaml; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "XFRM block policy smoke failed; yaml: $xfrm_yaml; log: $log"
    return 1
  fi
}

case_xfrm_runtime() {
  name="xfrm-runtime"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "root required; run with sudo or execute: sudo sh scripts/vm-evaluate.sh xfrm-runtime $YAML"
    return 0
  fi
  if sh "$ROOT_DIR/scripts/vm-xfrm-policy-runtime-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "scoped bidirectional XFRM block policy installation and cleanup passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "XFRM runtime policy smoke failed; log: $log"
    return 1
  fi
}

case_xfrm_cleartext() {
  name="xfrm-cleartext"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "root required; run with sudo: sudo sh scripts/vm-evaluate.sh xfrm-cleartext $YAML"
    return 0
  fi
  if sh "$ROOT_DIR/scripts/vm-xfrm-cleartext-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "encrypted direct traffic and post-SA cleartext blocking passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "encrypted/cleartext XFRM smoke failed; log: $log"
    return 1
  fi
}

case_fallback_time() {
  name="fallback-time"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  i=1
  total_ms=0
  : > "$log"
  while [ "$i" -le "$EVAL_REPEAT" ]; do
    iter_start=$(now_ns)
    iter_out="$OUT_DIR/fallback-runtime-$i"
    if (OUT_DIR="$iter_out" sh "$ROOT_DIR/scripts/vm-generate-netns-runtime.sh" "$YAML" --active-path path-direct --fail-path path-direct) >> "$log" 2>&1; then
      iter_ms=$(elapsed_ms "$iter_start")
      total_ms=$((total_ms + iter_ms))
      printf 'iteration=%s duration_ms=%s selected=path-via-hub\n' "$i" "$iter_ms" >> "$log"
    else
      record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "fallback runtime generation failed at iteration $i; log: $log"
      return 1
    fi
    i=$((i + 1))
  done
  avg_ms=$((total_ms / EVAL_REPEAT))
  record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "average_plan_generation_ms=$avg_ms repeat=$EVAL_REPEAT; log: $log"
}

case_explain() {
  name="explain"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  explain="$OUT_DIR/explain.jsonl"
  ensure_build
  if "$BUILD_DIR/eventnet_scenario" "$YAML" --step direct-ok --step direct-failed --step direct-recovered --step relay-best --explain-json "$explain" > "$log" 2>&1 &&
    grep -q '"selected_path":"path-direct"' "$explain" &&
    grep -q '"selected_path":"path-via-hub"' "$explain" &&
    grep -q '"selected_path":"path-via-relay-c"' "$explain" &&
    grep -q '"result":"pass"' "$explain"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "explain JSONL includes decision path, reason, and pass result; jsonl: $explain"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "explain replay check failed; log: $log"
    return 1
  fi
}

case_integrated_direct() {
  name="integrated-direct"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "0" "root required; run: sudo sh scripts/vm-evaluate.sh integrated-direct $YAML"
    return 0
  fi
  if (MODE=direct sh "$ROOT_DIR/scripts/vm-controller-integrated-runtime-smoke.sh" "$YAML") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "IPsec path and VPP forwarding passed in direct mode; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "integrated direct runtime failed; log: $log"
    return 1
  fi
}

case_integrated_fallback() {
  name="integrated-fallback"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "0" "root required; run: sudo sh scripts/vm-evaluate.sh integrated-fallback $YAML"
    return 0
  fi
  if (MODE=fallback sh "$ROOT_DIR/scripts/vm-controller-integrated-runtime-smoke.sh" "$YAML") > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "direct failure selected hub fallback and carried traffic; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "integrated fallback runtime failed; log: $log"
    return 1
  fi
}

case_transition_policy() {
  name="transition-policy"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if "$BUILD_DIR/eventnet_tests" > "$log" 2>&1 &&
    grep -q 'all tests passed' "$log"; then
    record_case "$name" "partial" "$(elapsed_ms "$start_ns")" "Immediate and Graceful transition behavior, draining, cleanup, and rollback are covered by C tests; Flow Preserve remains unimplemented; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "transition policy unit checks failed; log: $log"
    return 1
  fi
}

case_rollback() {
  name="rollback"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if "$BUILD_DIR/eventnet_tests" > "$log" 2>&1 &&
    grep -q 'all tests passed' "$log" &&
    (OUT_DIR="$OUT_DIR/rollback-runtime" sh "$ROOT_DIR/scripts/vm-generate-netns-runtime.sh" "$YAML" --active-path path-direct --fail-path path-direct) >> "$log" 2>&1 &&
    grep -q 'selected_path: path-via-hub' "$OUT_DIR/rollback-runtime/selected-path.txt" &&
    test -x "$OUT_DIR/rollback-runtime/rollback-selected.sh" &&
    grep -q 'vm-netns-ipsec-hub-stop.sh' "$OUT_DIR/rollback-runtime/rollback-selected.sh"; then
    record_case "$name" "partial" "$(elapsed_ms "$start_ns")" "controller rollback unit test, fallback plan, and explicit rollback executor generation passed; runtime failure injection remains; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "rollback-related fallback plan generation failed; log: $log"
    return 1
  fi
}

case_rollback_runtime() {
  name="rollback-runtime"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "root required; run with sudo or execute: sudo sh scripts/vm-evaluate.sh rollback-runtime $YAML"
    return 0
  fi
  if sh "$ROOT_DIR/scripts/vm-runtime-rollback-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "injected runtime failure cleaned direct IPsec state; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "runtime rollback smoke failed; log: $log"
    return 1
  fi
}

case_control_plane() {
  name="control-plane"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if sh "$ROOT_DIR/scripts/vm-eventnet-event-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "eventnetd file/socket failure-recovery, VICI config load, and explicit VRF route command ordering passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "eventnetd control-plane failure/recovery check failed; log: $log"
    return 1
  fi
}

case_restart_recovery() {
  name="restart-recovery"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  state="$OUT_DIR/eventnetd.state"
  telemetry="$OUT_DIR/recovery-telemetry.jsonl"
  symlink_result=0
  if "$BUILD_DIR/eventnet_agent" --path path-direct --source site-a --target 203.0.113.9 --count 1 --simulate 12 0 --output "$telemetry" > "$log" 2>&1 &&
    "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$telemetry" --state-file "$state" --once >> "$log" 2>&1 &&
    "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$telemetry" --state-file "$state" --once >> "$log" 2>&1 &&
    grep -q 'state_restored: 1' "$log"; then
    if [ "$(uname -s)" = "Linux" ]; then
      chmod 0666 "$state"
      if "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$telemetry" --state-file "$state" --once >> "$log" 2>&1; then
        symlink_result=1
      fi
      chmod 0600 "$state"
      mv "$state" "$state.target"
      ln -s "$(basename "$state.target")" "$state"
      if "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$telemetry" --state-file "$state" --once >> "$log" 2>&1; then
        symlink_result=1
      fi
      rm -f "$state" "$state.target"
    fi
    if [ "$symlink_result" -eq 0 ]; then
      record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "eventnetd persisted/restored applied Path and rejected a state-file symlink; log: $log"
    else
      record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "state-file symlink was accepted; log: $log"
      return 1
    fi
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "state persistence/recovery failed; log: $log"
    return 1
  fi
}

case_state_boundary() {
  name="state-boundary"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if ctest --test-dir "$BUILD_DIR" --output-on-failure -R '^eventnet_state_(wrong_(intent|path)|disabled_node)$' > "$log" 2>&1 &&
    grep -q '100% tests passed, 0 tests failed out of 3' "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "state restore rejected wrong-Intent, out-of-scope Path, and disabled-Node fixtures; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "state restore boundary checks failed or were not registered; log: $log"
    return 1
  fi
}

case_backend_reuse() {
  name="backend-reuse"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if "$BUILD_DIR/eventnet_tests" > "$log" 2>&1 &&
    grep -q 'all tests passed' "$log" &&
    sh "$ROOT_DIR/scripts/vm-generate-plan.sh" "$YAML" >> "$log" 2>&1 &&
    (OUT_DIR="$OUT_DIR/backend-netns-runtime" sh "$ROOT_DIR/scripts/vm-generate-netns-runtime.sh" "$YAML") >> "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "public strongSwan/VPP adapter contracts, shared selection core, swanctl plan, and VPP/netns plan passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "backend adapter or plan reuse smoke failed; log: $log"
    return 1
  fi
}

case_vlan_policy() {
  name="vlan-policy"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  vlan_yaml="${VLAN_YAML:-$ROOT_DIR/samples/vpp-vlan-netns.yaml}"
  vlan_out="$OUT_DIR/vlan-runtime"
  mkdir -p "$vlan_out"
  : > "$log"
  for vlan_id in ${VLAN_MATRIX:-"1 100 4094"}; do
    case "$vlan_id" in
      ''|*[!0-9]*) record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "invalid VLAN_MATRIX value: $vlan_id; log: $log"; return 1 ;;
    esac
    candidate_yaml="$vlan_out/vlan-$vlan_id.yaml"
    candidate_out="$vlan_out/out-$vlan_id"
    mkdir -p "$candidate_out"
    if ! sed "s/^      vlan_id: 100$/      vlan_id: $vlan_id/" "$vlan_yaml" > "$candidate_yaml" ||
      ! "$BUILD_DIR/eventnet_netns_plan" --intent intent-vpp-vlan --out-dir "$candidate_out" "$candidate_yaml" >> "$log" 2>&1 ||
      ! grep -q "^vlan_id: $vlan_id$" "$candidate_out/selected-path.txt" ||
      ! grep -q '^runtime_kind: vpp$' "$candidate_out/selected-path.txt" ||
      ! grep -q "create sub-interfaces .* $vlan_id" "$candidate_out/vpp-netns-route-plan.sh" ||
      ! grep -q "set interface .*\\.$vlan_id up" "$candidate_out/vpp-netns-route-plan.sh" ||
      ! grep -q "ip route add .*\\.$vlan_id" "$candidate_out/vpp-netns-route-plan.sh" ||
      ! grep -q "set acl-plugin acl index" "$candidate_out/vpp-netns-route-plan.sh" ||
      ! grep -q "set acl-plugin interface .* input acl" "$candidate_out/vpp-netns-route-plan.sh"; then
      record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VLAN $vlan_id policy plan check failed; log: $log"
      return 1
    fi
  done
  rejected_yaml="$vlan_out/rejected-vlan.yaml"
  rejected_out="$vlan_out/rejected-out"
  mkdir -p "$rejected_out"
  if ! sed -e 's/^      vlan_id: 100$/      vlan_id: 4094/' -e 's/^      - 4094$/      - 200/' "$vlan_yaml" > "$rejected_yaml" ||
    "$BUILD_DIR/eventnet_netns_plan" --intent intent-vpp-vlan --out-dir "$rejected_out" "$rejected_yaml" >> "$log" 2>&1; then
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VLAN allowlist rejection check failed: an unlisted VLAN was accepted; log: $log"
    return 1
  fi
  isolation_yaml="$ROOT_DIR/samples/vlan-vrf-isolation.yaml"
  isolation_out="$vlan_out/vrf-isolation"
  mkdir -p "$isolation_out/vlan-100" "$isolation_out/vlan-200"
  if ! "$BUILD_DIR/eventnet_netns_plan" --intent intent-vlan-100 --out-dir "$isolation_out/vlan-100" "$isolation_yaml" >> "$log" 2>&1 ||
    ! "$BUILD_DIR/eventnet_netns_plan" --intent intent-vlan-200 --out-dir "$isolation_out/vlan-200" "$isolation_yaml" >> "$log" 2>&1 ||
    ! grep -q '^    table: 100\r\?$' "$isolation_out/vlan-100/selected-path.txt" ||
    ! grep -q '^    table: 200\r\?$' "$isolation_out/vlan-200/selected-path.txt" ||
    grep -q 'table: 200' "$isolation_out/vlan-100/selected-path.txt" ||
    grep -q 'table: 100' "$isolation_out/vlan-200/selected-path.txt"; then
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VLAN VRF isolation plan mixed tables; log: $log"
    return 1
  fi
  hub_yaml="$ROOT_DIR/samples/vpp-vlan-hub-netns.yaml"
  hub_out="$vlan_out/hub-waypoint"
  mkdir -p "$hub_out"
  if ! "$BUILD_DIR/eventnet_netns_plan" --intent intent-vpp-vlan --out-dir "$hub_out" "$hub_yaml" >> "$log" 2>&1 ||
    ! grep -q 'ensure_vlan_subinterface host-vpp-site-a 100' "$hub_out/vpp-netns-route-plan.sh" ||
    ! grep -q 'ensure_vlan_subinterface host-vpp-hub-1 100' "$hub_out/vpp-netns-route-plan.sh" ||
    ! grep -q 'ensure_vlan_subinterface host-vpp-hub-b 100' "$hub_out/vpp-netns-route-plan.sh" ||
    ! grep -q 'ensure_unmatched_vlan_acl host-vpp-hub-1' "$hub_out/vpp-netns-route-plan.sh" ||
    ! grep -q 'ip route add 10.10.2.0/24 table 100 via 172.16.103.2 host-vpp-hub-b.100' "$hub_out/vpp-netns-route-plan.sh"; then
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VLAN Hub waypoint multi-port plan check failed; log: $log"
    return 1
  fi
  record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "VLAN matrix (${VLAN_MATRIX:-1 100 4094}), VLAN-to-VRF isolation, and Hub waypoint multi-port plans passed; log: $log"
}

case_node_capability() {
  name="node-capability"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  node_yaml="${NODE_CAPABILITY_YAML:-$ROOT_DIR/samples/node-capabilities.yaml}"
  if "$BUILD_DIR/eventnet_yaml_demo" "$node_yaml" --intent "$INTENT_ID" > "$log" 2>&1 &&
    grep -q "loaded nodes: 2" "$log" &&
    grep -q "node: site-a role=edge capabilities: ipsec vpp" "$log" &&
    grep -q "selected_path: path-a-b" "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "Node capability parsing and constrained path selection passed; yaml: $node_yaml; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "Node capability evaluation failed; yaml: $node_yaml; log: $log"
    return 1
  fi
}

case_route_yaml() {
  name="route-yaml"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if sh "$ROOT_DIR/scripts/vm-route-yaml-smoke.sh" "${ROUTE_YAML:-$ROOT_DIR/samples/route-examples.yaml}" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "all supported route YAML forms and invalid-route rejection passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "route YAML smoke failed; log: $log"
    return 1
  fi
}

case_reload_sighup() {
  name="reload-sighup"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "Linux is required for SIGHUP and POSIX process signaling; log: $log"
    return 0
  fi
  if sh "$ROOT_DIR/scripts/vm-eventnet-sighup-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "SIGHUP-triggered reload and invalid-config retention passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "SIGHUP reload smoke failed; log: $log"
    return 1
  fi
}

case_event_reconcile() {
  name="event-reconcile"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if sh "$ROOT_DIR/scripts/vm-eventnet-event-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "path failure/recovery file, Unix socket stream, and finite multi-Agent shared-batch passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "event reconcile smoke failed; log: $log"
    return 1
  fi
}

case_swanctl_observer() {
  name="swanctl-observer"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if "$BUILD_DIR/eventnet_swanctl_observer" "$ROOT_DIR/samples/swanctl-list-sas-installed.txt" tun-a-b > "$log" 2>&1 &&
    grep -q '^state: established$' "$log" &&
    grep -q '^health: healthy$' "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "swanctl CHILD SA output was converted to established/healthy Observed State; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "swanctl observer conversion failed; log: $log"
    return 1
  fi
}

case_vici_runtime() {
  name="vici-runtime"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  probe="${VICI_BUILD_DIR:-$ROOT_DIR/build-vici}/eventnet_strongswan_vici_probe"
  if [ ! -x "$probe" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "optional libvici probe is not built; configure with -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON and set VICI_BUILD_DIR to enable it."
    return 0
  fi
  if [ -z "${VICI_URI:-}" ] || [ -z "${VICI_CHILD:-}" ] || [ -z "${VICI_PATH:-}" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "set VICI_URI, VICI_CHILD, and VICI_PATH to run the real libvici evaluation; log: $log"
    return 0
  fi
  monitor="$OUT_DIR/vici-monitor.jsonl"
  if "$probe" "$VICI_URI" version > "$log" 2>&1 &&
    "$probe" "$VICI_URI" observe "$VICI_CHILD" >> "$log" 2>&1 &&
    "$probe" "$VICI_URI" monitor "$VICI_CHILD" "$VICI_PATH" "${VICI_DURATION_MS:-5000}" "${VICI_RETRY_COUNT:-2}" "${VICI_BACKOFF_MS:-250}" > "$monitor" 2>> "$log" &&
    ! grep -v '^$' "$monitor" | grep -v '^{"schema":"ibuki.event.tunnel.v1"' >/dev/null 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "libvici version/observe/finite monitor passed; JSONL output: $monitor; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "libvici runtime evaluation failed; log: $log"
    return 1
  fi
}

case_vici_controller() {
  name="vici-controller"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  probe="${VICI_CONTROLLER_PROBE:-${VICI_BUILD_DIR:-$ROOT_DIR/build-vici}/eventnet_strongswan_vici_controller_probe}"
  if [ ! -x "$probe" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "optional VICI controller probe is not built; configure with -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON."
    return 0
  fi
  if [ -z "${VICI_URI:-}" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "set VICI_URI and build the optional VICI controller probe; log: $log"
    return 0
  fi
  yaml="${VICI_CONTROLLER_YAML:-$ROOT_DIR/samples/cert-auth.yaml}"
  intent="${VICI_CONTROLLER_INTENT:-intent-cert-a-b}"
  if "$probe" "$VICI_URI" "$yaml" "$intent" > "$log" 2>&1 && grep -q '^VICI controller reconcile passed$' "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "real VICI client reached controller reconcile; yaml: $yaml; intent: $intent; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VICI controller reconcile failed; yaml: $yaml; intent: $intent; log: $log"
    return 1
  fi
}

case_vici_eventnetd_internal() {
  name="vici-eventnetd-internal"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  vici_build_dir="${VICI_BUILD_DIR:-$ROOT_DIR/build-vici}"
  if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "Linux is required for eventnetd internal VICI monitoring; log: $log"
    return 0
  fi
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "root required; run evaluation with sudo; log: $log"
    return 0
  fi
  if [ ! -x "$vici_build_dir/eventnetd" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "VICI-enabled eventnetd is not built; configure with -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON; log: $log"
    return 0
  fi
  if [ -z "${VICI_URI:-}" ] || [ -z "${VICI_CHILD:-}" ] || [ -z "${VICI_PATH:-}" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "set VICI_URI, VICI_CHILD, and VICI_PATH for internal VICI evaluation; log: $log"
    return 0
  fi
  if RUN_INTERNAL=1 VICI_BUILD_DIR="$vici_build_dir" VICI_URI="$VICI_URI" VICI_CHILD="$VICI_CHILD" VICI_PATH="$VICI_PATH" \
    VICI_DURATION_MS="${VICI_DURATION_MS:-5000}" YAML="$YAML" OUT_DIR="$OUT_DIR/vici-internal" \
    sh "$ROOT_DIR/scripts/vm-vici-eventnetd-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "eventnetd internal VICI monitor, stop condition, and reconcile passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "eventnetd internal VICI smoke failed; log: $log"
    return 1
  fi
}

case_vpp_observer() {
  name="vpp-observer"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  if "$BUILD_DIR/eventnet_vpp_observer" "$ROOT_DIR/samples/vpp-show-ip-fib.txt" 10.10.2.0/24 > "$log" 2>&1 &&
    grep -q '^present: yes$' "$log" &&
    grep -q '^next_hop: 203.0.113.9$' "$log" &&
    "$BUILD_DIR/eventnet_vpp_observer" "$ROOT_DIR/samples/vpp-show-ip-fib-vrf.txt" 10.10.2.0/24 --event path-direct route-vrf --table 100 > "$OUT_DIR/vpp-vrf-event.jsonl" 2>> "$log" &&
    grep -q '"table_id":100' "$OUT_DIR/vpp-vrf-event.jsonl" &&
    grep -q '"tunnel_id":"route-vrf"' "$OUT_DIR/vpp-vrf-event.jsonl" &&
    ! grep -q '198.51.100.1' "$OUT_DIR/vpp-vrf-event.jsonl" &&
    cat "$OUT_DIR/vpp-vrf-event.jsonl" | "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry-stdin --count 1 > "$OUT_DIR/vpp-vrf-event.log" 2>> "$log" &&
    grep -q 'selected_path: path-direct' "$OUT_DIR/vpp-vrf-event.log" &&
    sed 's/203.0.113.9/192.0.2.1/' "$OUT_DIR/vpp-vrf-event.jsonl" > "$OUT_DIR/vpp-vrf-event-mismatch.jsonl" &&
    ! "$BUILD_DIR/eventnetd" "$YAML" --intent intent-a-b --telemetry "$OUT_DIR/vpp-vrf-event-mismatch.jsonl" --once > "$OUT_DIR/vpp-vrf-mismatch.log" 2>&1 &&
    grep -q 'telemetry identity rejected: telemetry route does not match path route' "$OUT_DIR/vpp-vrf-mismatch.log" &&
    ! "$BUILD_DIR/eventnet_vpp_observer" "$ROOT_DIR/samples/vpp-show-ip-fib-vrf.txt" 10.10.2.0/24 --event path-direct route-vrf --table -1 > /dev/null 2>> "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "VPP FIB output and explicit VRF table selection were converted to route Observed State; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VPP observer conversion failed; log: $log"
    return 1
  fi
}

case_vpp_api() {
  name="vpp-api"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "Linux is required for VPP Binary API dependency detection; log: $log"
    return 0
  fi
  if sh "$ROOT_DIR/scripts/vm-vpp-api-preflight.sh" > "$log" 2>&1; then
    if [ "${RUN_VPP_API_DISCOVER:-1}" = "1" ]; then
      if ! OUT_FILE="$OUT_DIR/vpp-api-discovery.txt" sh "$ROOT_DIR/scripts/vm-vpp-api-discover.sh" >> "$log" 2>&1; then
        record_case "$name" "partial" "$(elapsed_ms "$start_ns")" "VPP Binary API preflight passed but discovery failed; inspect SDK manually; log: $log"
        return 0
      fi
    fi
    if [ "${RUN_VPP_API:-0}" = "1" ]; then
      probe="${VPP_API_PROBE:-$BUILD_DIR/eventnet_vpp_api_transport_probe}"
      if [ ! -x "$probe" ]; then
        record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VPP Binary API dependencies are present but transport probe is missing; rebuild with EVENTNET_ENABLE_VPP_API=ON; log: $log"
        return 1
      fi
      if "$probe" >> "$log" 2>&1; then
        record_case "$name" "partial" "$(elapsed_ms "$start_ns")" "VPP Binary API transport connected and closed; generated route/VLAN message transport remains incomplete; log: $log"
      else
        record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "VPP Binary API transport probe failed; log: $log"
        return 1
      fi
    else
      record_case "$name" "partial" "$(elapsed_ms "$start_ns")" "VPP Binary API dependencies are present; set RUN_VPP_API=1 to execute the no-change transport probe; generated route/VLAN message transport remains incomplete; log: $log"
    fi
  else
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "VPP Binary API dependencies are unavailable; vppctl adapter remains the supported backend; log: $log"
  fi
}

case_observer_runtime() {
  name="observer-runtime"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(id -u)" != "0" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "root required; start IPsec/VPP runtime first, then run: sudo sh scripts/vm-evaluate.sh observer-runtime $YAML"
    return 0
  fi
  if INTENT_ID="$INTENT_ID" sh "$ROOT_DIR/scripts/vm-observer-eventnetd-runtime-smoke.sh" "$YAML" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "real strongSwan and VPP observer events reached eventnetd with route identity validation; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "real observer event integration failed; log: $log"
    return 1
  fi
}

case_cert_auth() {
  name="cert-auth"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  ensure_build
  cert_dir="$OUT_DIR/certificates"
  mkdir -p "$cert_dir"
  if ! command -v openssl >/dev/null 2>&1; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "openssl is required for reproducible certificate validation; log: $log"
    return 0
  fi
  if openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
      -subj /CN=ibuki-evaluation \
      -keyout "$cert_dir/key.pem" -out "$cert_dir/site-a.crt" > "$log" 2>&1 &&
    cp "$cert_dir/site-a.crt" "$cert_dir/example-ca.crt" &&
    sed "s#site-a.crt#$cert_dir/site-a.crt#; s#example-ca.crt#$cert_dir/example-ca.crt#" \
      "$ROOT_DIR/samples/cert-auth.yaml" > "$cert_dir/config.yaml" &&
    "$BUILD_DIR/eventnet_yaml_demo" "$cert_dir/config.yaml" --check-cert-files --check-cert-validity 86400 >> "$log" 2>&1 &&
    grep -q 'certificate validity: at least 86400 seconds' "$log"; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "pubkey certificate readability and 24-hour validity window passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "certificate validation failed; log: $log"
    return 1
  fi
}

case_plan_secret_permission() {
  name="plan-secret-permission"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "Linux is required for O_NOFOLLOW and mode 0600 checks; log: $log"
    return 0
  fi
  if OUT_DIR="$OUT_DIR/$name" sh "$ROOT_DIR/scripts/vm-plan-secret-permission-smoke.sh" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "generated swanctl.conf permission and symlink protection passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "generated swanctl.conf protection failed; log: $log"
    return 1
  fi
}

case_status_output_security() {
  name="status-output-security"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  if [ "$(uname -s 2>/dev/null || printf unknown)" != "Linux" ]; then
    record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "Linux is required for O_NOFOLLOW and mode checks; log: $log"
    return 0
  fi
  if BUILD_DIR="$BUILD_DIR" sh "$ROOT_DIR/scripts/vm-eventnetd-status-security-smoke.sh" "$YAML" > "$log" 2>&1; then
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "eventnetd status output protection passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "eventnetd status output protection failed; log: $log"
    return 1
  fi
}

case_service_unit() {
  name="service-unit"
  start_ns=$(now_ns)
  log="$OUT_DIR/$name.log"
  unit="$ROOT_DIR/deploy/ibuki-eventnetd.service"
  if [ ! -r "$unit" ]; then
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "systemd unit template is missing; log: $log"
    return 1
  fi
  if grep -q '^Type=simple$' "$unit" &&
    grep -q '^User=ibuki$' "$unit" &&
    grep -q '^Group=ibuki$' "$unit" &&
    grep -q '^Restart=on-failure$' "$unit" &&
    grep -q '^NoNewPrivileges=yes$' "$unit" &&
    grep -q '^ProtectSystem=strict$' "$unit" &&
    grep -q '^PrivateTmp=yes$' "$unit" &&
    grep -q '^PrivateDevices=yes$' "$unit" &&
    grep -q '^ProtectKernelTunables=yes$' "$unit" &&
    grep -q '^ProtectKernelModules=yes$' "$unit" &&
    grep -q '^ProtectControlGroups=yes$' "$unit" &&
    grep -q '^RestrictSUIDSGID=yes$' "$unit" &&
    grep -q '^LockPersonality=yes$' "$unit" &&
    grep -q '^RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6$' "$unit" &&
    grep -q '^RuntimeDirectory=ibuki$' "$unit" &&
    grep -q '^StateDirectory=ibuki$' "$unit" &&
    grep -q '^UMask=0077$' "$unit" &&
    grep -q '^ReadWritePaths=/run/ibuki /var/lib/ibuki$' "$unit" &&
    grep -q -- '--socket-accept-count 0' "$unit" &&
    ! grep -q -- '--apply' "$unit" &&
    grep -q '^ExecStartPre=' "$unit" > "$log" 2>&1; then
    if command -v systemd-analyze >/dev/null 2>&1; then
      systemd-analyze verify "$unit" >> "$log" 2>&1 || {
        record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "systemd-analyze rejected the unit; log: $log"
        return 1
      }
    fi
    record_case "$name" "pass" "$(elapsed_ms "$start_ns")" "systemd unit safety contract passed; log: $log"
  else
    record_case "$name" "fail" "$(elapsed_ms "$start_ns")" "systemd unit safety contract failed; log: $log"
    return 1
  fi
}

case_federation() {
  name="federation"
  start_ns=$(now_ns)
  record_case "$name" "skip" "$(elapsed_ms "$start_ns")" "multi-domain Transition Proposal protocol is documented but not implemented in this prototype."
}

case_all() {
  overall=0
  run_evaluation_case() {
    "$1" || overall=1
  }
  run_evaluation_case case_scenario
  run_evaluation_case case_telemetry
  run_evaluation_case case_telemetry_long
  run_evaluation_case case_telemetry_live
  run_evaluation_case case_threshold
  run_evaluation_case case_stability
  run_evaluation_case case_xfrm_policy
  run_evaluation_case case_fallback_time
  run_evaluation_case case_explain
  run_evaluation_case case_transition_policy
  run_evaluation_case case_rollback
  run_evaluation_case case_backend_reuse
  run_evaluation_case case_control_plane
  run_evaluation_case case_restart_recovery
  run_evaluation_case case_state_boundary
  run_evaluation_case case_vlan_policy
  run_evaluation_case case_node_capability
  run_evaluation_case case_route_yaml
  run_evaluation_case case_reload_sighup
  run_evaluation_case case_event_reconcile
  run_evaluation_case case_swanctl_observer
  run_evaluation_case case_vici_runtime
  run_evaluation_case case_vici_controller
  run_evaluation_case case_vici_eventnetd_internal
  run_evaluation_case case_vpp_observer
  run_evaluation_case case_vpp_api
  run_evaluation_case case_cert_auth
  run_evaluation_case case_plan_secret_permission
  run_evaluation_case case_status_output_security
  run_evaluation_case case_service_unit
  run_evaluation_case case_federation
  if [ "$RUN_RUNTIME" = "1" ]; then
    run_evaluation_case case_xfrm_runtime
    run_evaluation_case case_xfrm_cleartext
    run_evaluation_case case_rollback_runtime
    run_evaluation_case case_integrated_direct
    run_evaluation_case case_integrated_fallback
  else
    record_case "integrated-runtime" "skip" "0" "set RUN_RUNTIME=1 and run as root to include IPsec + VPP runtime checks."
  fi
  unset -f run_evaluation_case
  return "$overall"
}

start_report

cd "$ROOT_DIR"
case "$CASE" in
  -h|--help|help)
    usage
    ;;
  all)
    case_all
    ;;
  scenario)
    case_scenario
    ;;
  telemetry)
    case_telemetry
    ;;
  telemetry-long)
    case_telemetry_long
    ;;
  telemetry-live)
    case_telemetry_live
    ;;
  threshold)
    case_threshold
    ;;
  stability)
    case_stability
    ;;
  xfrm-policy)
    case_xfrm_policy
    ;;
  xfrm-runtime)
    case_xfrm_runtime
    ;;
  xfrm-cleartext)
    case_xfrm_cleartext
    ;;
  fallback-time)
    case_fallback_time
    ;;
  explain)
    case_explain
    ;;
  integrated-direct)
    case_integrated_direct
    ;;
  integrated-fallback)
    case_integrated_fallback
    ;;
  transition-policy)
    case_transition_policy
    ;;
  rollback)
    case_rollback
    ;;
  rollback-runtime)
    case_rollback_runtime
    ;;
  control-plane)
    case_control_plane
    ;;
  restart-recovery)
    case_restart_recovery
    ;;
  state-boundary)
    case_state_boundary
    ;;
  vlan-policy)
    case_vlan_policy
    ;;
  node-capability)
    case_node_capability
    ;;
  route-yaml)
    case_route_yaml
    ;;
  reload-sighup)
    case_reload_sighup
    ;;
  event-reconcile)
    case_event_reconcile
    ;;
  swanctl-observer)
    case_swanctl_observer
    ;;
  vici-runtime)
    case_vici_runtime
    ;;
  vici-controller)
    case_vici_controller
    ;;
  vici-eventnetd-internal)
    case_vici_eventnetd_internal
    ;;
  vpp-observer)
    case_vpp_observer
    ;;
  vpp-api)
    case_vpp_api
    ;;
  observer-runtime)
    case_observer_runtime
    ;;
  cert-auth)
    case_cert_auth
    ;;
  plan-secret-permission)
    case_plan_secret_permission
    ;;
  status-output-security)
    case_status_output_security
    ;;
  service-unit)
    case_service_unit
    ;;
  backend-reuse)
    case_backend_reuse
    ;;
  federation)
    case_federation
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac

trap - EXIT
finish_report 0
