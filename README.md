# Ibuki

Ibukiは、複数の拠点間通信経路を選択し、安全に切り替えるためのイベント駆動型ネットワークControllerです。C言語で実装されており、YAMLで記述されたIntent、Path、Tunnel、Nodeなどを読み込み、strongSwanとVPPを利用する実行計画を生成します。

本プロジェクトは、単に「最も速い経路を選ぶ」ことだけを目的としません。通信経路を状態を持つ`Path`として扱い、準備、検証、転送変更、切替後確認、安定化、Rollback、Fallbackまでを一つの制御モデルで扱うことを目的としています。

現在は研究・実証用のPrototypeです。本番Networkへそのまま導入できる完成製品ではありません。

## 1. Ibukiでできること

現在の実装では、主に次の処理を行えます。

- YAMLから`Node`、`Intent`、`Path`、`Tunnel`、`VPP Edge`を読み込む。
- 明示指定、優先度、Healthや性能値の評価によってPathを選択する。
- 利用できないPathや条件を満たさないPathを候補から除外する。
- AgentがRTT、Packet Loss、Jitterなどを測定し、Telemetry JSONLを生成する。
- Telemetryや障害Eventを受けてPathを再評価する。
- strongSwan用設定とVPP用Route Planを生成する。
- 選択理由、除外理由、観測値、遷移結果をExplain JSONLへ記録する。
- Direct Path、Hub Fallback、Relay PathなどのScenarioを再現する。
- Linux Network Namespace上でIPsecとVPPを組み合わせた試験を行う。

GUI、Cloud VPN連携、完全なController Federation、Active-Active転送、Graceful Transition、Flow Preserveは今後の実装対象です。

## 2. 基本的な設計方針

### 2.1 Pathを制御の中心に置く

Ibukiでは、TunnelやRouteを個別に操作するだけでなく、通信に必要なTunnel、Segment、Waypoint、Routeをまとめた`Path`を制御対象とします。

これにより、Controllerは「Tunnelが存在するか」だけでなく、「この通信要求に対して、どの経路が現在利用可能で、どの経路が実際に使用中か」を管理します。

### 2.2 Path SelectionとPath Transitionを分ける

Path Selectionは、どのPathを利用するかを決める処理です。Path Transitionは、選択されたPathへ実際の通信を移す処理です。

新しいPathが選ばれても、Tunnelが未確立であったり、転送変更後にEnd-to-End通信が成立しなかったりする可能性があります。そのため、Ibukiでは「選んだこと」と「安全に切り替えられたこと」を同一視しません。

### 2.3 Stableを疎通確認後の状態とする

転送設定を変更しただけでは、そのPathを`Stable`とは扱いません。新しいPathを通るEnd-to-End通信を確認し、必要な安定条件を満たした後に`Stable`へ移行します。

代表的な状態は次のとおりです。

```text
Proposed
  -> Preparing
  -> Validating
  -> Ready
  -> Commit Executing
  -> Commit Applied
  -> Post Validation
  -> Stable
```

失敗時には`Rolling Back`、`Fallbacking`、`Failed`などへ移行します。

### 2.4 RollbackとFallbackを分ける

Rollbackは、切替前に利用していた最後の`Stable Path`へ戻す処理です。

Fallbackは、元のPathも利用できない場合に、あらかじめ安全経路として許可されたHub Pathなどへ退避する処理です。

この二つを分けることで、「変更を取り消すこと」と「障害から避難すること」を混同しないようにします。

### 2.5 Security Policyを性能より優先する

Pathの選択では、原則として次の順序で条件を扱います。

1. Security Constraint
2. 明示された管理者Policy
3. AvailabilityとHealth
4. RTT、Loss、Hop Countなどの性能条件

例えば、VLAN 100の通信にSecurity Hubの経由が必要であれば、RTTが短くてもSecurity Hubを含まないDirect Pathは選択しません。

### 2.6 Backendの違いをCapabilityとして公開する

strongSwan、VPP、既存IPsec Router、Cloud VPNでは、事前確立、状態観測、経路変更、Rollbackなどの能力が異なります。

Ibukiでは、これらの差を隠して同一機能に見せるのではなく、AdapterとCapabilityによってControllerへ公開します。

Capabilityの例は次のとおりです。

- `can_observe_state`
- `can_pre_establish`
- `can_keep_standby`
- `can_rekey`
- `can_measure_health`
- `can_flow_preserve`
- `can_atomic_forwarding_update`

Controllerは、必要なCapabilityを持たないNodeやPathを候補から除外します。

### 2.7 判断理由を記録する

Controllerの判断は、Explain JSONLとして記録します。GUIは将来実装ですが、GUIが表示する判断根拠となるBackendはすでにExplain出力として用意されています。

Explainには、少なくとも次の情報を残します。

- 選択されたPath
- 候補となったPath
- 候補から除外した理由
- 判断に利用したHealthと性能値
- 遷移前後の状態
- Commit、Validation、Rollback、Fallbackの結果

### 2.8 障害時にも現在の通信を不用意に壊さない

Controllerとの通信が失われても、Agentは現在のActive Pathを直ちに削除しません。Active Path自体が利用不能になった場合に限り、事前に許可されたEmergency Fallbackを利用する設計とします。

## 3. 動作環境

基本的なBuildとUnit TestはWindowsでも実行できます。strongSwan、VPP、Network Namespaceを利用する実通信試験はLinux VMを前提とします。

主な依存関係は次のとおりです。

- C Compiler
- CMake
- CTest
- Linux VMでの実通信試験時: strongSwan、VPP、iproute2、Root権限
- 任意機能: strongSwan libvici、VPP VAPI SDK

## 4. 最初に試す方法

### 4.1 Linux VM

Repositoryの`controller`Directoryへ移動し、Shell Scriptの確認、Build、Scenario試験を順番に実行します。

```sh
cd controller
sh scripts/vm-shell-check.sh
sh scripts/vm-build-cc.sh
sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml
sh scripts/vm-evaluate.sh state-boundary samples/linux-vm-netns.yaml
```

最小Demoは次のCommandで実行できます。

```sh
sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

strongSwanとVPPを含む実Runtimeを適用する場合はRoot権限が必要です。

```sh
sudo RUN_RUNTIME=1 sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

このCommandはNetwork設定を変更するため、専用のLinux VMまたは検証環境で実行してください。

### 4.2 Windows

```powershell
cd controller
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

作業Folderを移動した場合、既存の`build/`に古い絶対Pathを含む`CMakeCache.txt`が残ることがあります。その場合は既存Build Directoryを再利用せず、別のDirectoryを指定してください。

```powershell
cmake -S . -B build-win-check
cmake --build build-win-check
ctest --test-dir build-win-check -C Debug --output-on-failure
```

## 5. 基本的な使い方

### 5.1 YAMLを確認する

最初は[`samples/linux-vm-netns.yaml`](samples/linux-vm-netns.yaml)を使用してください。このYAMLには、Direct、Hub、Relayの候補Pathと、それらを選択するIntentが含まれています。

YAMLだけを検証する場合は次を実行します。

```sh
build-linux-cc/eventnet_yaml_demo --validate-only samples/linux-vm-netns.yaml
```

Ibukiの主な設定要素は次のとおりです。

| 要素 | 役割 |
| --- | --- |
| `nodes` | 拠点、Hub、Relay、Capability、管理状態を定義する |
| `tunnels` | Endpoint、Traffic Selector、認証情報などを定義する |
| `vpp_edges` | VPP Interface、Address、Next Hop、Portを定義する |
| `paths` | Source、Destination、Segment、Waypoint、Routeをまとめる |
| `intents` | 対象通信、Path選択方式、制約、遷移、Fallbackを定義する |

YAMLのRoute記法は[`docs/yaml-routes.md`](docs/yaml-routes.md)を参照してください。

### 5.2 Pathを選択する

YAMLからControllerが選択したPlanを生成します。

```sh
sh scripts/vm-generate-plan.sh samples/linux-vm-netns.yaml
```

障害時の選択を再現する場合はScenario Runnerを利用します。

```sh
build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub
```

この例では、使用中の`path-direct`を障害状態にし、`path-via-hub`が選択されることを確認します。

### 5.3 AgentでHealthを測定する

YAMLからIntentの候補Pathと測定先を読み取り、Telemetry JSONLを生成します。

```sh
build-linux-cc/eventnet_agent \
  --yaml samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --count 10 \
  --interval-ms 1000 \
  --output out/telemetry.jsonl
```

AgentはPathごとに次の値を出力します。

- `rtt_ms`
- `packet_loss_percent`
- `jitter_ms`
- 連続成功回数
- 連続失敗回数

実NetworkへPingせず、値を指定して制御処理だけを確認することもできます。

```sh
build-linux-cc/eventnet_agent \
  --path path-direct \
  --target 203.0.113.9 \
  --count 1 \
  --simulate 12.5 0
```

### 5.4 ControllerへTelemetryを渡す

一回だけ評価する場合は次を実行します。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --telemetry out/telemetry.jsonl \
  --once
```

判断結果をJSONLへ保存する場合は`--status-jsonl`を指定します。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --telemetry out/telemetry.jsonl \
  --count 10 \
  --status-jsonl out/status.jsonl
```

Controller再起動後も適用済みPathを引き継ぐ場合はState Fileを指定します。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --telemetry out/telemetry.jsonl \
  --state-file out/eventnetd.state \
  --once
```

### 5.5 AgentからControllerへ直接渡す

Agentの標準出力をControllerの標準入力へ接続できます。

```sh
build-linux-cc/eventnet_agent \
  --path path-direct \
  --target 203.0.113.9 \
  --count 10 \
  --interval-ms 1000 \
  | build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
      --intent intent-a-b \
      --telemetry-stdin \
      --count 10
```

LinuxではUnix Domain SocketからTelemetryを受け取ることもできます。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --telemetry-socket /run/ibuki/eventnetd.sock \
  --count 10
```

現在のSocket入力はLocal Agent用の境界です。認証済みのRemote APIとしてInternetへ公開しないでください。

### 5.6 strongSwanとVPPの実行Planを生成する

選択されたPathに対応するNetwork Namespace用Runtimeを生成します。

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml
```

主な生成物は`out/`以下に作成されます。

| 生成物 | 内容 |
| --- | --- |
| `selected-path.txt` | 選択Path、理由、Route概要 |
| `gre-swanctl.conf` | strongSwan設定 |
| `vpp-route-plan.sh` | VPP Route計画 |
| `vpp-netns-route-plan.sh` | Node別VPP計画 |
| `apply-selected.sh` | 選択Pathの適用Script |
| `apply-integrated.sh` | strongSwanとVPPの統合適用Script |
| `rollback-selected.sh` | 失敗時のRollback Script |

生成ScriptをRoot権限で実行する前に、必ず内容を確認してください。

### 5.7 設定を再読み込みする

FileからTelemetryを読む`eventnetd`は、LinuxでSIGHUPによる安全な設定再読込を行えます。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --intent intent-a-b \
  --telemetry out/telemetry.jsonl \
  --reload-config \
  --reload-on-sighup \
  --state-file out/eventnetd.state \
  --count 0
```

別Terminalから次を実行します。

```sh
kill -HUP <eventnetd-pid>
```

新しい設定が不正な場合は、直前に正常だった設定を維持します。

## 6. Path選択方式

Intentでは主に次の選択方式を利用できます。

| Mode | 用途 |
| --- | --- |
| `explicit` | 管理者がPath IDを明示する |
| `priority` | 利用可能な候補のうちAdministrative Priorityを比較する |
| `evaluated` | RTT、Loss、Hop Count、Priorityなどを指定順に比較する |

ConstraintとしてRTTやPacket Lossの上限を指定できます。Node Capability、Administrative State、Health、Waypointなどの条件を満たさないPathは、性能比較を行う前に候補から除外します。

同じ優先度の強制Policyが競合する場合、Controllerが暗黙に一方を選ぶのではなく、Intent Conflictとして扱い、Explainへ理由を残す方針です。

## 7. 開発時の基本方針

### 7.1 実装済みと設計段階を分ける

文書、発表、Issueでは、次の状態を区別してください。

- 実通信で確認済み
- Unit TestまたはMockで確認済み
- Plan生成まで実装済み
- Adapter/API境界のみ実装済み
- 設計済みだが未実装
- 将来構想

例えば、Explain JSONLのBackendは実装済みですが、GUI Frontendは未実装です。VPP CLI Adapterは利用できますが、VPP Binary APIの全機能が完成しているわけではありません。

### 7.2 Backend固有処理をCoreへ入れない

strongSwan、VPP、FRR、Cloud VPNなどの固有処理はAdapterへ配置します。Path SelectionやTransitionのCoreが、特定BackendのCommandやData Structureへ直接依存しないようにします。

### 7.3 観測値と推測値を分ける

Tunnel Segmentの状態からPath全体の利用可能性を予測することと、End-to-End通信が成功したことは別です。

```text
Segment Observation
  -> Path Prediction
  -> End-to-End Verification
```

`Stable`判定には可能な限りEnd-to-End Verificationを使用します。

### 7.4 Network変更後に状態を確認する

Commandの終了Codeだけで成功と判断しないでください。特に`vppctl`はCLI Errorが発生してもProcessの終了Codeだけでは検出できない場合があります。

VPP変更後は、少なくとも次を確認します。

```sh
show ipsec sa
show ipsec protect
show interface
show ip fib
```

strongSwan変更後は、CHILD SAの状態と実際のEnd-to-End通信を確認します。

### 7.5 生成物をSourceとして編集しない

`out/`と`build*/`以下は生成物です。必要に応じて再生成し、恒久的な変更は`src/`、`include/`、`examples/`、`scripts/`、`samples/`などのSource側へ反映してください。

### 7.6 検証は小さい範囲から行う

変更後は、対象Unit Test、Scenario Test、Plan生成、Network Namespace試験、実strongSwan/VPP試験の順で確認します。Root権限が必要な試験を最初から実行せず、まず非特権で確認できる範囲を通してください。

### 7.7 秘密情報をRepositoryへ保存しない

実運用のPSK、秘密鍵、証明書秘密鍵、Cloud CredentialをSample YAMLや生成物へCommitしないでください。Sampleでは検証専用の値を使い、本番Credentialは権限を制限した外部Storeから渡す方針です。

## 8. Repository構成

| Directory | 内容 |
| --- | --- |
| `include/eventnet/` | 公開C API、Data Model、Adapter Contract |
| `src/` | Controller Core、Parser、State、Adapter実装 |
| `examples/` | `eventnetd`、Agent、Scenario、Plan Generator等のEntry Point |
| `tests/` | Unit Testと統合寄りのTest |
| `samples/` | YAML、Telemetry、Observer出力、異常系Fixture |
| `scripts/` | Build、Demo、評価、strongSwan/VPP試験Script |
| `docs/` | 設計、運用、Schema、研究比較、実装状況 |
| `deploy/` | systemd等の配置用File |
| `out/` | 生成されたPlan、状態、Log、評価結果 |
| `build*/` | Build結果 |

詳しいSource FileとFunctionの対応は[`docs/code-reference.md`](docs/code-reference.md)、Shell Command一覧は[`docs/shell-commands.md`](docs/shell-commands.md)を参照してください。

## 9. 関連Document

### 利用・運用

- [`docs/yaml-routes.md`](docs/yaml-routes.md): YAML Route記法とValidation Rule
- [`docs/event-schemas.md`](docs/event-schemas.md): Telemetry、Event、Status、Explain JSONL
- [`docs/eventnetd-service.md`](docs/eventnetd-service.md): `eventnetd`の常駐運用
- [`docs/transport-adapter-guide.md`](docs/transport-adapter-guide.md): strongSwan VICI、VPP CLI/Binary API
- [`scripts/README-linux-vm.md`](scripts/README-linux-vm.md): Linux VMでの構築・試験手順

### 設計・実装状況

- [`実装方針.md`](実装方針.md): Ibuki全体の実装方針
- [`docs/future-implementation-map.md`](docs/future-implementation-map.md): 今後の実装場所と優先順位
- [`docs/scenario-vs-production.md`](docs/scenario-vs-production.md): Scenarioと本番Controllerの差
- [`docs/security-audit-notes.md`](docs/security-audit-notes.md): Trust BoundaryとSecurity上の注意
- [`docs/vpp-api-implementation.md`](docs/vpp-api-implementation.md): VPP Binary APIの実装状況

### 研究・評価

- [`研究内容.tex`](研究内容.tex): 研究論文Source
- [`docs/prior-research-and-sdwan2.md`](docs/prior-research-and-sdwan2.md): 先行研究とONUG SD-WAN 2.0への対応状況
- [`docs/asano-comparison.md`](docs/asano-comparison.md): ASANO Systemとの比較
- [`docs/paper-evaluation-checklist.md`](docs/paper-evaluation-checklist.md): 論文評価で必要な証拠
- [`docs/paper-submission-minimum.md`](docs/paper-submission-minimum.md): 論文提出時の最低条件
- [`docs/paper-to-mitou-roadmap.md`](docs/paper-to-mitou-roadmap.md): 論文から未踏期間へのRoadmap

## 10. 現在の位置付け

Ibukiは、ONUGが示すSD-WAN 2.0の全機能を実装した製品ではありません。一方で、複数Pathの管理、PolicyとHealthに基づくPath Selection、標準IPsecによる異種装置との接続、Security Waypoint、状態を伴うPath Transition、Adapter/Capability Modelという設計は、SD-WAN 2.0の中核的な方向性と整合します。

現時点では、次のように位置付けます。

> Ibukiは、SD-WAN内部のPath切替を、状態、能力、失敗回復、説明可能性の観点から分解し、比較・再現・評価できるようにするPath制御研究基盤である。

本番利用には、Credential Lifecycle、Command実行境界の追加Hardening、常駐Serviceの運用検証、Controller HA、Remote Agent認証、Cloud API連携、GUIなどが必要です。

## 11. License

License条件は[`LICENSE`](LICENSE)を参照してください。
