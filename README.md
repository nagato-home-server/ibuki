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

### Top-level directories

| path | responsibility | how to use it |
| --- | --- | --- |
| `include/eventnet/` | Public C API, data model, and adapter contracts | Include these headers from applications or tests linking `eventnet_controller` |
| `src/` | Controller core, parser, state machine, observers, and external adapters | Built as the `eventnet_controller` static library by CMake |
| `examples/` | Executable entry points | Built into `eventnetd`, `eventnet_agent`, plan generators, observers, and probes |
| `tests/` | Controller unit and integration-style C tests | Run through `ctest --test-dir build --output-on-failure` |
| `cmake/` | Assertions for generated YAML/runtime plans | Invoked by CTest; normally not run directly |
| `samples/` | Valid, invalid, replay, and fixture inputs | Pass a YAML or JSONL file to the matching executable or smoke test |
| `scripts/` | Build, setup, evaluation, strongSwan, XFRM, and VPP orchestration | See `docs/shell-commands.md`; scripts changing networking require root |
| `docs/` | Design, schema, operation, implementation status, and security documentation | Read `code-reference.md` for code and `shell-commands.md` for commands |
| `deploy/` | Service-manager integration | Install `ibuki-eventnetd.service` through CMake install or copy it for packaging |
| `third_party/yyjson/` | Vendored JSON parser | Compiled into the library; do not modify for Ibuki-specific behavior |
| `story/`, `daily/`, `txt/` | Design history, daily notes, and proposal research | Reference material; not used by the build or runtime |
| `out/`, `build*/` | Generated plans, logs, reports, and binaries | Regenerate as needed; these are not runtime source files |

### Top-level files

| file | responsibility |
| --- | --- |
| `CMakeLists.txt` | Builds the core library, executables, optional VICI/VAPI transports, CTest cases, and install targets |
| `README.md` | Project overview, basic operation, repository map, and documentation entry point |
| `LICENSE` | Project license terms |
| `実装方針.md` | Current implementation policy and research-to-code decisions |
| `研究内容.tex` | Research manuscript source; not part of the program build |
| `ibuki-github-qr.png` | Presentation/document asset; not used at runtime |

### Public headers

| file | responsibility |
| --- | --- |
| `types.h` | Node, Path, Segment, Tunnel, Intent, health, transition, and error data structures |
| `controller.h` | Controller lifecycle, health submission, reconciliation, and result APIs |
| `yaml_config.h` | Ibuki YAML subset loader and validator |
| `topology.h` | Topology and route relationship helpers |
| `telemetry.h` | Telemetry JSONL parsing, validation, and file handling |
| `apply_plan.h` | Apply-plan data structures and secure plan-file output |
| `render_commands.h` | strongSwan/VPP command and configuration rendering |
| `command_adapters.h` | Command-backed strongSwan, VPP, and health adapter factories |
| `mock_adapters.h` | Deterministic adapters used by tests and scenarios |
| `strongswan_observer.h` | Conversion of strongSwan state into controller observations |
| `strongswan_vici_adapter.h` | Controller-facing strongSwan VICI adapter contract |
| `strongswan_vici_client.h` | Optional real VICI client contract |
| `vpp_observer.h` | Conversion of VPP CLI output into route/interface observations |
| `vpp_api_adapter.h` | Controller-facing VPP Binary API adapter contract |
| `vpp_api_transport.h` | Optional VPP VAPI transport contract |
| `json_output.h` | Shared safe JSONL serialization helpers |

### Library implementation files

| file | responsibility |
| --- | --- |
| `controller.c` | Main reconciliation entry point: observe, select, transition, audit |
| `path_selection.c` | Intent constraints, fallback, priority, and evaluated Path selection |
| `transition.c` | Prepare, apply, commit, drain, and rollback state transitions |
| `state.c` | Applied-Path state, lookup helpers, traffic keys, and enum names |
| `topology.c` | Path, segment, waypoint, and topology consistency operations |
| `health_probe.c` | Health-probe validation and adapter integration |
| `yaml_config.c` | Line-oriented parser for the supported YAML subset and semantic validation |
| `telemetry.c` | JSONL health/event parsing, freshness, identity, and sequence checks |
| `apply_plan.c` | Secure writing and application boundaries for generated plans |
| `render_commands.c` | strongSwan and VPP command/configuration generation |
| `command_adapters.c` | Real command execution, dry-run, verification, XFRM, VLAN, VRF, and route handling |
| `strongswan_observer.c` | Parses and normalizes strongSwan/CHILD_SA observations |
| `strongswan_vici_adapter.c` | Connects controller callbacks to the VICI client abstraction |
| `strongswan_vici_client.c` | Optional libvici-backed transport, compiled only when enabled |
| `vpp_observer.c` | Parses VPP FIB and interface output |
| `vpp_api_adapter.c` | Maps controller VPP operations to a Binary API transport |
| `vpp_api_transport.c` | Optional VAPI connection and request implementation |
| `strongswan_adapter_mock.c` | In-memory strongSwan behavior for tests |
| `vpp_adapter_mock.c` | In-memory VPP behavior for tests |
| `audit.c` | Bounded audit and error history |
| `json_output.c` | yyjson-to-JSONL output implementation |
| `internal.h` | Private cross-module declarations; not a public API |

Function-level ownership and side effects are documented in `docs/code-reference.md`.

### Tests, CMake checks, and deployment

| file | responsibility | operation |
| --- | --- | --- |
| `tests/test_controller.c` | Unit tests for parsing, selection, transitions, state, adapters, and security boundaries | `ctest --test-dir build -R eventnet_tests --output-on-failure` |
| `cmake/check-vlan-vrf-isolation.cmake` | Checks generated VLAN/VRF isolation plans | Invoked by `eventnet_yaml_vlan_vrf_isolation` CTest |
| `cmake/check-vlan-hub-plan.cmake` | Checks waypoint/hub VLAN route plans | Invoked by `eventnet_yaml_vlan_hub_waypoint` CTest |
| `cmake/check-gre-over-ipsec-plan.cmake` | Checks GRE-over-IPsec plan generation | Invoked by `eventnet_gre_over_ipsec_plan` CTest |
| `cmake/check-namespace-runtime.cmake` | Checks generated apply, integrated, and rollback runtime files | Invoked by `eventnet_namespace_runtime_plan` CTest |
| `deploy/ibuki-eventnetd.service` | systemd unit template for a resident controller | See `docs/eventnetd-service.md` before installation |

### Executables and operation

All examples below assume a Linux `vm-build-cc.sh` build. For CMake builds, replace `build-linux-cc/` with the selected build directory.

| source / executable | responsibility | typical operation |
| --- | --- | --- |
| `eventnetd.c` / `eventnetd` | Long-running or one-shot controller process | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry samples/telemetry-replay.jsonl --once` |
| `eventnet_agent.c` / `eventnet_agent` | Endpoint probing and Telemetry JSONL generation | `build-linux-cc/eventnet_agent --yaml samples/linux-vm-netns.yaml --intent intent-a-b --count 1 --simulate 12.5 0` |
| `netns_plan.c` / `eventnet_netns_plan` | Generates selected Path, strongSwan, VPP, apply, and rollback files | `build-linux-cc/eventnet_netns_plan --intent intent-a-b --out-dir out/netns-runtime samples/linux-vm-netns.yaml` |
| `eventnet_scenario.c` / `eventnet_scenario` | Reproducible failure, fallback, recovery, and relay scenarios | `build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml --step direct-failed` |
| `yaml_demo.c` / `eventnet_yaml_demo` | YAML validation and simple plan generation | `build-linux-cc/eventnet_yaml_demo --validate-only samples/linux-vm-netns.yaml` |
| `demo.c` / `eventnet_demo` | Minimal library API example | `build-linux-cc/eventnet_demo` |
| `swanctl_observer.c` / `eventnet_swanctl_observer` | Parses saved `swanctl --list-sas` output | `build-linux-cc/eventnet_swanctl_observer samples/swanctl-list-sas-installed.txt CHILD_ID` |
| `vpp_observer.c` / `eventnet_vpp_observer` | Parses a saved VPP FIB | `build-linux-cc/eventnet_vpp_observer samples/vpp-show-ip-fib.txt PREFIX` |
| `vpp_interface_observer.c` / `eventnet_vpp_interface_observer` | Parses saved VPP interface state | `build-linux-cc/eventnet_vpp_interface_observer samples/vpp-show-interface.txt INTERFACE` |
| `strongswan_vici_probe.c` | Direct libvici transport probe | Build with `-DEVENTNET_ENABLE_STRONGSWAN_VICI=ON`, then run its `version`, `observe`, or `monitor` action |
| `strongswan_vici_controller_probe.c` | Exercises VICI through the controller adapter | Build with VICI enabled, then pass `VICI_URI YAML [INTENT_ID]` |
| `vpp_api_transport_probe.c` | Checks VPP VAPI connection and transport availability | Build with `-DEVENTNET_ENABLE_VPP_API=ON`, then run `eventnet_vpp_api_transport_probe` |

### Samples and fixtures

| group | files | use |
| --- | --- | --- |
| Main topology | `linux-vm-netns.yaml`, `node-capabilities.yaml`, `route-examples.yaml` | Normal scenario, selection, and route-form testing |
| VPP routing | `vpp-netns-routes.yaml`, `vpp-vlan-netns.yaml`, `vpp-vlan-hub-netns.yaml` | FIB, VRF, VLAN, ACL, and waypoint plan tests |
| GRE/IPsec | `gre-over-ipsec.yaml`, `gre-namespace-v2.yaml`, `gre-vpp-data-plane.yaml` | strongSwan GRE and VPP data-plane experiments |
| VPP Native IPsec | `gre-namespace-v2-vpp-native.yaml` | Experimental IPIP plus `ipsec tunnel protect` plan; inspect postconditions before relying on it |
| Certificate/IPsec | `cert-auth.yaml`, `ipsec-routes.yaml` | Certificate and route-based IPsec generation |
| Compatibility YAML | `agent-legacy-no-segment.yaml` | Legacy route derivation and Agent compatibility testing |
| Negative YAML | `route-invalid-examples.yaml`, `invalid-empty-no-segment.yaml`, `legacy-explicit-no-segment.yaml`, `vlan-vrf-conflict.yaml` | Validation must fail for these inputs |
| Isolation YAML | `vlan-vrf-isolation.yaml` | Positive VLAN/VRF isolation plan fixture |
| Telemetry replay | `telemetry-*.jsonl` | Deterministic `eventnetd` input and fallback testing |
| Persisted-state fixtures | `state-*.tsv`, `state-disabled-node.yaml` | State-boundary and restart-safety tests |
| Observer fixtures | `vpp-show-*.txt`, `swanctl-list-sas-installed.txt` | Parser testing without a running daemon |
| Evaluation data | `paper-metrics-template.csv` | Paper/evaluation metric collection template |

### Script entry points

Use these high-level scripts first. The lower-level scripts they call, all arguments, and environment variables are listed in `docs/shell-commands.md`.

| purpose | entry point |
| --- | --- |
| Dependency and syntax checks | `vm-check.sh`, `vm-runtime-status.sh`, `vm-shell-check.sh` |
| Build | `vm-build-cc.sh`, `vm-build.sh` |
| One-shot demonstration | `demo-mitou.sh` |
| Plan generation | `vm-generate-plan.sh`, `vm-generate-netns-runtime.sh` |
| Scenario and controller tests | `vm-eventnet-scenario-smoke.sh`, `vm-netns-controller-smoke.sh` |
| Full evaluation | `vm-evaluate.sh`, `vm-paper-validation.sh`, `vm-paper-collect-metrics.sh` |
| Namespace underlay | `vm-netns-setup.sh`, `vm-netns-smoke.sh`, `vm-netns-clean.sh` |
| strongSwan IPsec | `vm-netns-ipsec.sh direct|hub|gre <action>` |
| VPP preparation | `vm-vpp-preflight.sh`, `vm-install-vpp-fdio.sh` |
| Per-namespace VPP | `vm-vpp-ns-topology.sh`, `vm-vpp-ns-runtime.sh` |
| VPP/controller integration | `vm-vpp-controller-netns-smoke.sh`, `vm-controller-integrated-runtime-smoke.sh` |
| GRE and Native IPsec experiments | `vm-gre-over-ipsec-smoke.sh`, `vm-gre-namespace-v2-smoke.sh`, `vm-gre-vpp-data-smoke.sh` |

### Generated runtime files

`eventnet_netns_plan` and the wrapper scripts create the following files under `out/`.

| file | responsibility |
| --- | --- |
| `selected-path.txt` | Selected Path, reason, and route summary |
| `gre-swanctl.conf` | strongSwan connection and CHILD_SA configuration |
| `vpp-route-plan.sh` | VPP route-only plan |
| `vpp-netns-route-plan.sh` | Per-node VPP socket, interface, SA, tunnel, and route plan |
| `apply-selected.sh` | Applies the selected runtime |
| `apply-integrated.sh` | Applies the combined strongSwan and VPP runtime |
| `rollback-selected.sh` | Removes the selected runtime after failure or explicit rollback |

Generate them with:

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml
```

Inspect the generated files before running them with `DRY_RUN=0` or root privileges.

### VPP operation notes

- `vppctl` transports CLI text but does not reliably convert a VPP CLI error into a non-zero process exit status. Do not treat `$? == 0` as sufficient verification.
- Verify state after mutation with `show ipsec sa`, `show ipsec protect`, `show interface`, and `show ip fib`.
- `create ipip tunnel ... del` is not an IPIP delete command. Current VPP uses `delete ipip tunnel sw_if_index <index>`.
- Each VPP instance must contain both the outbound and inbound SA referenced by `ipsec tunnel protect`.
- `gre-namespace-v2-vpp-native.yaml` is an experimental diagnostic path. The current generated plan must not be considered successful unless `show ipsec protect` contains the expected interface and both SAs.
- VPP CLI and Binary API sockets are management boundaries. Keep them as restricted Unix sockets and do not expose an unauthenticated TCP CLI.

## Documentation

### Code and operation

| document | contents |
| --- | --- |
| `docs/code-reference.md` | C files, important functions, ownership, side effects, and execution paths |
| `docs/shell-commands.md` | Complete `scripts/*.sh` command, argument, environment-variable, and purpose table |
| `scripts/README-linux-vm.md` | Ordered Linux VM setup and smoke-test procedure |
| `docs/eventnetd-service.md` | Resident `eventnetd` systemd deployment and operation |
| `docs/transport-adapter-guide.md` | strongSwan VICI and VPP CLI/Binary API transport guide |
| `docs/vpp-api-implementation.md` | VPP Binary API implementation status and remaining work |
| `docs/yaml-routes.md` | Supported YAML route forms and validation rules |
| `docs/event-schemas.md` | Versioned Telemetry, event, status, and explain JSONL contracts |

### Runtime and architecture

| document | contents |
| --- | --- |
| `docs/namespace-runtime-v2.md` | Namespace, direct/hub/GRE IPsec, VPP, and Native IPsec runtime experiments |
| `docs/gre-data-plane-fork.md` | GRE data-plane alternatives and design decision points |
| `docs/gre-namespace-constraint.md` | GRE and network namespace constraints |
| `docs/scenario-vs-production.md` | Scenario harness behavior versus production controller requirements |
| `docs/security-audit-notes.md` | Trust boundaries, local audit findings, and prioritized hardening |

### Planning and evaluation

| document | contents |
| --- | --- |
| `docs/mitou-submission-status.md` | Current implementation status for the MITOU submission |
| `docs/future-implementation-map.md` | Next implementation targets and their file/function locations |
| `docs/worker-guide.md` | Contributor workflow, ownership, outputs, and handoff guidance |
| `docs/paper-evaluation-checklist.md` | Evaluation evidence required for the paper |
| `docs/paper-submission-minimum.md` | Minimum implementation and evidence for submission |
| `docs/paper-to-mitou-implementation-plan.md` | Implementation sequence from paper prototype to MITOU project |
| `docs/paper-to-mitou-roadmap.md` | Longer-term research and implementation roadmap |

### Comparative research

| document | contents |
| --- | --- |
| `docs/awesome-mitou-comparison.md` | Comparison with public `awesome-mitou` materials |
| `docs/asano-comparison.md` | Comparison notes against the referenced Asano work |

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
