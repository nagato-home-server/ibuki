#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
trap 'exit 1' INT TERM
mkdir -p "$TEST_DIR/bin"
COMMAND_LOG="$TEST_DIR/commands.log"
export COMMAND_LOG
cat > "$TEST_DIR/bin/id" <<'EOF'
#!/usr/bin/env sh
printf '%s\n' "${MOCK_UID:-0}"
EOF
cat > "$TEST_DIR/bin/ip" <<'EOF'
#!/usr/bin/env sh
printf 'ip %s\n' "$*" >> "$COMMAND_LOG"
exit "${MOCK_IP_STATUS:-0}"
EOF
cat > "$TEST_DIR/bin/vppctl" <<'EOF'
#!/usr/bin/env sh
printf 'vppctl %s\n' "$*" >> "$COMMAND_LOG"
exit "${MOCK_VPP_STATUS:-0}"
EOF
cat > "$TEST_DIR/bin/swanctl" <<'EOF'
#!/usr/bin/env sh
printf 'swanctl %s\n' "$*" >> "$COMMAND_LOG"
EOF
chmod +x "$TEST_DIR/bin/"*
PATH="$TEST_DIR/bin:$PATH"
export PATH
cd "$ROOT_DIR"
case_count=0

expect_pass() {
  "$@" > "$TEST_DIR/output.log" 2>&1 || {
    cat "$TEST_DIR/output.log" >&2
    exit 1
  }
  case_count=$((case_count + 1))
}

expect_fail() {
  if "$@" > "$TEST_DIR/output.log" 2>&1; then
    printf 'Unexpected success: %s\n' "$*" >&2
    exit 1
  fi
  case_count=$((case_count + 1))
}

for script in vm-netns.sh vm-vpp-netns.sh vm-netns-ipsec.sh; do
  expect_fail sh "scripts/$script"
  expect_fail sh "scripts/$script" invalid
done

for mode in direct hub; do
  expect_pass env OUT_DIR="$TEST_DIR/$mode" PSK=test-only-dispatch-key sh scripts/vm-netns-ipsec.sh "$mode" generate
  test -s "$TEST_DIR/$mode/site-a/swanctl.conf"
  test -s "$TEST_DIR/$mode/site-b/swanctl.conf"
  grep -q 'test-only-dispatch-key' "$TEST_DIR/$mode/site-a/swanctl.conf"
  expect_fail env OUT_DIR="$TEST_DIR/rejected" PSK=change-me sh scripts/vm-netns-ipsec.sh "$mode" generate
  expect_fail sh scripts/vm-netns-ipsec.sh "$mode" invalid
  expect_fail env MOCK_UID=1000 sh scripts/vm-netns-ipsec.sh "$mode" start
  expect_pass env RUN_BASE="$TEST_DIR/run-$mode" SWANCTL_WORK_BASE="$TEST_DIR/work-$mode" sh scripts/vm-netns-ipsec.sh "$mode" status
  expect_pass env RUN_BASE="$TEST_DIR/run-$mode" SWANCTL_WORK_BASE="$TEST_DIR/work-$mode" sh scripts/vm-netns-ipsec.sh "$mode" stop
  expect_pass env RUN_BASE="$TEST_DIR/run-$mode" sh scripts/vm-netns-ipsec.sh "$mode" clean
done
test -s "$TEST_DIR/hub/hub-1/swanctl.conf"

for action in generate smoke invalid; do
  expect_fail sh scripts/vm-netns-ipsec.sh gre "$action"
done
expect_fail sh scripts/vm-netns-ipsec.sh gre start "$TEST_DIR/custom-plan"
grep -q "missing GRE swanctl config: $TEST_DIR/custom-plan/gre-swanctl.conf" "$TEST_DIR/output.log"
mkdir -p "$TEST_DIR/run-gre/site-a"
printf 'custom-gre-log\n' > "$TEST_DIR/run-gre/site-a/charon.log"
expect_pass env GRE_RUN_BASE="$TEST_DIR/run-gre" sh scripts/vm-netns-ipsec.sh gre logs
grep -q custom-gre-log "$TEST_DIR/output.log"
expect_pass env GRE_RUN_BASE="$TEST_DIR/run-gre" GRE_SWANCTL_WORK_BASE="$TEST_DIR/work-gre" sh scripts/vm-netns-ipsec.sh gre stop

expect_pass sh scripts/vm-netns.sh setup
grep -q 'ip netns add relay-c' "$COMMAND_LOG"
expect_pass sh scripts/vm-netns.sh clean
grep -q 'ip netns del client-a' "$COMMAND_LOG"
expect_fail env MOCK_UID=1000 sh scripts/vm-netns.sh setup
expect_fail env MOCK_IP_STATUS=7 sh scripts/vm-netns.sh setup

expect_pass sh scripts/vm-vpp-netns.sh setup
expect_pass env VPP_TOPOLOGY=hub sh scripts/vm-vpp-netns.sh setup
grep -q 'vppctl create host-interface name vpp-hub-a' "$COMMAND_LOG"
expect_pass sh scripts/vm-vpp-netns.sh status
expect_pass sh scripts/vm-vpp-netns.sh clean
expect_fail env VPP_TOPOLOGY=invalid sh scripts/vm-vpp-netns.sh setup
expect_fail env MOCK_UID=1000 sh scripts/vm-vpp-netns.sh setup
expect_fail env MOCK_VPP_STATUS=9 sh scripts/vm-vpp-netns.sh setup

printf 'Shell dispatch tests passed: %s cases (mock commands, no network changes).\n' "$case_count"
