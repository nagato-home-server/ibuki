# Ibuki

PathWeaver is an event-driven IPsec path controller written in C.

It reads YAML intents, selects a usable VPN path, and generates runtime plans for
strongSwan and VPP.  The current prototype can reproduce direct IPsec, hub
fallback, relay path selection, VPP forwarding, and integrated controller-driven
runtime switching in a single Linux VM.

## What Works Today

- YAML-based `Intent` / `Path` / `Tunnel` / VPP edge parsing
- Priority, fallback, and evaluated path selection
- Explain JSONL output for selected paths, excluded paths, and health inputs
- Agent telemetry JSONL with freshness, sequence, identity, and anti-flap handling
- `eventnetd` periodic, stdin, Unix socket, reconnect, and finite parallel shared-batch control
- Versioned JSONL contracts are documented in `docs/event-schemas.md`
- strongSwan `swanctl.conf` and apply script generation
- VPP route plan and VPP netns runtime generation
- Linux network namespace smoke tests for direct, hub, and relay paths
- Integrated IPsec + VPP runtime smoke driven by controller-generated plans
- Scenario harness for direct failure, hub fallback, recovery, and relay-best cases
- systemd service template for long-running `eventnetd` deployment (`docs/eventnetd-service.md`)

## Quick Start

### Linux VM Demo

Use this first when running from the shared Linux VM folder.

```sh
cd controller
sh scripts/vm-shell-check.sh
sh scripts/vm-build-cc.sh
sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml
sh scripts/vm-evaluate.sh state-boundary samples/linux-vm-netns.yaml
```

One-shot demo:

```sh
sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

Full runtime demo with IPsec/VPP requires root and the runtime dependencies:

```sh
sudo RUN_RUNTIME=1 sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

### Windows Build

```powershell
cd controller
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

作業フォルダを移動した後や共有フォルダを切り替えた後は、既存の`build/`が古い絶対パスの`CMakeCache.txt`を持つことがあります。その場合は既存buildを再利用せず、`cmake -S . -B build-win-check`のように新しい検証用ディレクトリを構成してから、同じbuild・CTest手順を実行してください。

## Basic Usage

Generate a controller-selected plan from YAML:

```sh
sh scripts/vm-generate-plan.sh samples/linux-vm-netns.yaml
```

For a node with multiple VPP ports, give each `vpp_edges` entry a unique
`port_id` and select the port from an explicit route with `interface`. The
Hub waypoint example is reproducible with:

```sh
sh scripts/vm-vpp-route-plan-smoke.sh samples/linux-vm-netns.yaml
ctest --test-dir build -R eventnet_yaml_vlan_hub_waypoint --output-on-failure
```

Run scenario tests:

```sh
build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub
```

Run the Agent telemetry smoke test:

```sh
sh scripts/vm-agent-smoke.sh
```

Probe a real IPv4 endpoint from Linux and emit telemetry JSONL:

```sh
build-linux-cc/eventnet_agent --path path-direct --source site-a \
  --target 203.0.113.9 --count 10 --interval-ms 1000 \
  --output out/telemetry.jsonl
```

Probe several candidate paths in each measurement round:

```sh
build-linux-cc/eventnet_agent --source site-a \
  --probe path-direct 203.0.113.9 \
  --probe path-via-hub 203.0.113.13 \
  --probe path-via-relay-c 203.0.113.21 \
  --count 10 --interval-ms 1000 --output out/telemetry.jsonl
```

Derive candidate paths and endpoints directly from the YAML:

```sh
build-linux-cc/eventnet_agent --yaml samples/linux-vm-netns.yaml \
  --intent intent-a-b --count 10 --interval-ms 1000 \
  --output out/telemetry.jsonl
```

The Agent emits `rtt_ms`, `packet_loss_percent`, and per-Path RTT-difference `jitter_ms` in each JSONL record.

Feed Agent telemetry into the controller once:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl --once
```

Store machine-readable decisions for evaluation:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl --count 10 \
  --status-jsonl out/status.jsonl
```

Persist the applied Path across daemon restarts:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl \
  --state-file out/eventnetd.state --once
```

Show the strongSwan/VPP command backend without applying changes:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl \
  --backend command --once
```

Target a namespace-specific strongSwan VICI socket:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl \
  --backend command --swanctl-uri unix:///run/eventnet-netns-ipsec-direct/site-a/charon.vici --once
```

Target a VPP CLI socket as well:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl --backend command \
  --swanctl-uri unix:///run/eventnet-netns-ipsec-direct/site-a/charon.vici \
  --vppctl-socket /run/vpp/cli.sock --once
```

Ask the command backend to verify the CHILD SA after initiation:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl --backend command \
  --swanctl-uri unix:///run/eventnet-netns-ipsec-direct/site-a/charon.vici \
  --verify-swanctl --once
```

When libvici is available, build the optional controller-level VICI probe:

```sh
cmake -S . -B build-vici -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON
cmake --build build-vici
build-vici/eventnet_strongswan_vici_controller_probe \
  unix:///run/strongswan/charon.vici samples/cert-auth.yaml intent-cert-a-b
```

This path uses the real strongSwan VICI socket through the controller adapter.
The probe uses a mock VPP adapter, so VPP forwarding is verified separately by
the integrated runtime and VPP netns smoke tests.

Feed real strongSwan and VPP observations into one controller evaluation (Linux VM):

```sh
sudo sh scripts/vm-observer-eventnetd-runtime-smoke.sh samples/linux-vm-netns.yaml
```

For a VPP VRF, select the FIB table explicitly:

```sh
sudo INTENT_ID=intent-a-b VPP_TABLE_ID=100 sh scripts/vm-observer-eventnetd-runtime-smoke.sh samples/linux-vm-netns.yaml
```

The IPsec VICI socket and VPP CLI must already be running. The smoke stores raw observer input and eventnetd output under `out/observer-eventnetd-runtime/`.

Test VLAN and VRF together on the Linux VM:

```sh
sudo VLAN_ID=100 VPP_TABLE_ID=100 sh scripts/vm-vpp-vlan-netns-smoke.sh
```

Repeat telemetry evaluation:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl \
  --interval-ms 5000 --count 10
```

Run a reproducible periodic Agent-to-controller stream evaluation:

```sh
LONG_COUNT=10 LONG_INTERVAL_MS=1000 \
  sh scripts/vm-evaluate.sh telemetry-long samples/linux-vm-netns.yaml
```

The evaluation records the number of reconciliations, status JSONL output, and the persisted state file.

`state-boundary` verifies that state from another Intent or an out-of-scope Path is rejected. Linux builds enable baseline hardening by default; set `EVENTNET_ENABLE_HARDENING=OFF` only for diagnostic builds.

Reload a file-backed configuration on demand (Linux):

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b --telemetry out/telemetry.jsonl \
  --reload-config --reload-on-sighup --state-file out/eventnetd.state --count 0
# from another terminal:
kill -HUP <eventnetd-pid>
```

Evaluate every supported YAML route form:

```sh
sh scripts/vm-evaluate.sh route-yaml samples/route-examples.yaml
```

Stream one Agent record directly into the controller:

```sh
build-linux-cc/eventnet_agent --path path-direct --target 203.0.113.9 --count 10 --interval-ms 1000 \
  | build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b \
    --telemetry-stdin --count 10
```

For one multi-Path measurement round, batch records before selecting:

```sh
build-linux-cc/eventnet_agent --source site-a \
  --probe path-direct 203.0.113.9 --probe path-via-hub 203.0.113.13 \
  --probe path-via-relay-c 203.0.113.21 --simulate 20 0 \
  | build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
    --intent intent-a-b --telemetry-stdin --batch-size 3 --count 1
```

On Linux, receive Agent JSONL over a Unix domain socket:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b \
  --telemetry-socket /run/ibuki/eventnetd.sock --count 10
```

The daemon can accept one or more local Unix-socket clients, with optional peer-UID checking and finite parallel shared batches. This remains a local telemetry boundary rather than a complete authenticated production API.

Generate netns runtime files for the selected path:

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml
```

## Repository Map

- `include/eventnet/` - public C headers and controller model definitions
- `src/` - controller core, YAML parser, adapters, renderers, transition logic
- `examples/` - CLI/demo entry points and scenario harness
- `tests/` - C tests for controller behavior
- `samples/` - sample YAML configs
- `scripts/` - Linux VM setup, build, smoke, IPsec, and VPP helpers
- `docs/` - implementation status, roadmap, worker guide, and security notes
- `daily/` - daily development notes

## Documentation

- `docs/mitou-submission-status.md` - current implementation status for MITOU submission
- `docs/future-implementation-map.md` - next implementation targets and file/function map
- `docs/worker-guide.md` - guide for other contributors, functions, files, and outputs
- `docs/shell-commands.md` - `scripts/*.sh` execution examples, arguments, and environment variables
- `docs/scenario-vs-production.md` - scenario harness vs future production `eventnetd`
- `docs/security-audit-notes.md` - local security audit notes and priority fixes
- `docs/transport-adapter-guide.md` - VICI / VPP実transport接続ガイド
- `docs/awesome-mitou-comparison.md` - comparison with `awesome-mitou`
- `scripts/README-linux-vm.md` - full Linux VM smoke-test procedure

## Current Scope

PathWeaver currently focuses on the controller layer:

- intent and path selection
- YAML route definition
- strongSwan/VPP command and runtime generation
- Linux VM reproducible testing
- event/scenario experimentation

GUI, FRRouting integration, full production daemonization, and advanced flow
preservation are planned for later stages.

## Status

This is an early prototype for research and demonstration.  It is not yet a
production-ready network controller.

Before production use, the project still needs safer command execution through
API/argv-based transports, certificate and secret lifecycle management, CI,
complete service hardening, resident VICI event ingestion, and a production VPP
Binary API transport.

Core build and shell syntax checks are also defined in
`.github/workflows/ci.yml`. Runtime checks that require root, strongSwan, or
VPP remain explicit Linux VM evaluations.
