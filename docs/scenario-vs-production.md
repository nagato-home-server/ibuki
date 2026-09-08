# Scenario Harness と Production eventnetd の差分

この文書は、次に追加する `eventnet_scenario` の位置付けを明確にするためのものです。

`eventnet_scenario` は本番daemonではありません。Path selection、fallback、recovery、evaluated policyを安全かつ高速に実験するためのテストハーネスです。

最小のproduction入口として `eventnetd` も追加しています。現時点では、YAMLとAgent JSONLを読み込み、`--once` 相当の1回評価、`--interval-ms` / `--count` による周期評価、標準入力またはLinux Unixソケットによる逐次評価を実行します。有限接続では `--socket-parallel` により複数Agentを同時受信し、同一Controller状態へ集約できます。Backendはmockを既定値とし、command adapterをdry-runまたは明示的な`--apply`で選べます。strongSwanの操作は`swanctl --uri`を介して接続先を指定でき、`--swanctl-config`を併用するとconnection設定をloadしてからTunnelを開始できます。libvici有効ビルドでは`--vici-monitor-child`／`--vici-monitor-path`により、eventnetd自身がstrongSwanの`child-updown`を購読し、同じControllerへ再reconcileできます。VPPのCLI socket、VRF table準備、VLAN sub-interfaceの存在確認・作成・up、route投入、任意のFIB検証もcommand adapterへ接続できます。VPP Binary APIは接続、受信FD、dispatch、generic callback、生成messageの所有権境界まで実装済みですが、route／VLAN／VRFの具体codecは対象SDK版を固定した後段階として残しています。

Agentの逐次入力には `--telemetry-stdin` を使えます。`--telemetry FILE`もJSONL streamとして扱い、`--batch-size N`指定時はN件ごとに1回reconcileします。`--count C`はC回のbatch評価を要求し、各batchでN件を消費します。EOF時にN件未満となったbatchは適用せず、要求したC batchに到達しなければ終了コード1になります。Linuxでは `--telemetry-socket PATH` も使え、JSONL 1行ごとに同じController状態を更新して再評価します。複数Pathを同一測定ラウンドとして扱う場合は `--batch-size N` を指定し、N件を反映してから1回だけ再評価します。ソケットは既定では1接続ですが、`--socket-accept-count N --state-file PATH`により複数Agentを同一共有batchへ集約できます（N>1、有限接続）。共有batchの入力は4MiBを上限とし、`--socket-parallel`を付けるとN接続を同時pollで読み取り、`--socket-parallel-timeout-ms`超過時はreconcileせず終了します。`--socket-accept-count 0`なら無期限に逐次接続を受け付けます。Linuxでは`--socket-uid UID`で接続元UIDを検証できます。認証はUIDのみです。

file入力の周期実行では `--reload-config --state-file PATH` を指定すると、各周期の開始時にYAMLを再読込します。再読込に失敗した周期は適用せず終了し、前回の適用Pathはstate fileから復元します。stdin／socket入力との組み合わせは受け付けません。LinuxではSIGTERM／SIGINTを全入力モードで受け付け、直前に保存済みのstateを保持して終了します。`vm-eventnet-event-smoke.sh`でfile周期入力を停止させ、state fileが残ることを再現できます。

障害イベントの最小形式は `{"schema":"ibuki.event.path.v1","event":"path_failed|path_recovered","path_id":"...","timestamp_ms":...}` です。`eventnetd`はこれをhealth更新として扱うため、既存のPath選択・fallback・recovery処理を共有できます。Linux Unix socketでは、同一接続から複数のイベントスナップショットをbatch単位で連続処理できます。`--socket-accept-count N`（N>1）では有限個のAgent接続を共有batchへ集約し、全入力を一つのController状態で再評価します。

strongSwan／VPPの将来の購読元は、それぞれ `ibuki.event.tunnel.v1` または `ibuki.event.vpp.route.v1` の `path_id`、`tunnel_id`、`state`、`timestamp_ms`へ変換できます。`installed`／`rekeying`／`up`はhealthy、`deleted`／`down`／`failed`はfailedとして既存reconcileへ入力します。
observerのeventモードから`eventnetd --telemetry-stdin`へパイプするend-to-end smokeも用意しています。
Linux VMでは`vm-observer-eventnetd-runtime-smoke.sh`により、実際のstrongSwan VICI socketからの`--list-sas`出力とVPPの`show ip fib`出力をそれぞれobserver eventへ変換し、同じ`eventnetd`へ投入できます。これは常駐VICI購読ではありませんが、実runtimeの観測値が共通telemetry境界とPath identity検証を通過することを確認します。

通常のhealth入力で受理する状態値は `healthy`、`degraded`、`failed`、`unhealthy` に限定し、未知の値は入力エラーとして扱います。入力が高頻度になり得る環境では `--max-records-per-second N` を指定して、1秒あたりのレコード数を制限できます。既定値0は無制限です。

telemetry parserはPath IDの許可文字、metricの有限値・範囲、timestampを検証し、壊れたJSONLや不正な測定値をPath Selectionへ渡しません。
file入力でも空行以外はv1 schemaを要求し、schema欠落・未知schema・数値後ろの不正文字は明示的に拒否します。入力を読み飛ばして部分的に評価することはありません。

VLAN付きIntentでは、生成されたVPP netns planにsub-interface作成と有効化を含めます。これはVPP CLI計画の段階であり、実環境のparent interface、権限、既存設定との整合性はLinux VMで確認する必要があります。
VPP observerは`show interface`からVLAN sub-interfaceの存在と`up`状態も解析できます。

VLAN telemetryはroute観測と対象sub-interface観測を同一Pathへ集約します。両方のup観測が揃わない限りhealthyとは判定せず、未観測またはdownの場合はfallback対象にします。
標準入力／逐次socketの常駐処理では、観測が別batchで到着した一時的な`no_candidate`でdaemonを終了せず、次のbatchを待ちます。全batchが失敗した場合だけ非0終了になります。
`deny_unmatched_vlan: true`を指定したIntentでは、生成planとcommand backendがVPP親interfaceへIPv4/IPv6 deny-all ACLを適用し、指定sub-interface以外の未タグ通信を遮断する構成も選べます。VLAN smokeでは親interfaceにも別のL3経路を設定してから未タグpingを実行し、単なる未設定経路ではなくACLによる拒否を確認します。`vpp_edges.allowed_vlans`を指定したedgeでは一覧外VLANのsub-interface作成・route投入を拒否します。VLANごとのFIB table割当と設定生成は実装済みですが、実trunk上の複数VLAN疎通とVPPでの実パケット分離は未検証です。

現行のVLAN Policyは、指定VLANをtraffic keyとPath selectionへ結び付け、指定VLANのsub-interfaceだけへrouteを生成します。加えて`block_non_ipsec: true`を指定したIntentでは、Tunnel selector範囲に限定した双方向XFRM block policyを生成し、command backendのapply経路でも同じblockをTunnel単位で追加・削除します。`deny_unmatched_vlan: true`を指定したIntentでは、VPP親interfaceへIPv4/IPv6 deny-all ACLを適用し、未タグ通信を遮断できます。さらに`vpp_edges.allowed_vlans`でedge単位の許可VLANを制限できます。したがって、同一source/destinationでもVLANごとに選択PathとFIB計画を分離し、IPsec対象範囲のcleartext fallbackも抑止できます。一方、実trunk上の複数VLAN疎通とVPPでの実パケット分離は未検証です。論文前の実験では「許可VLANの疎通」「一覧外VLANのplan拒否」「deny ACLによる未タグ通信の遮断」「XFRM block planとcommand backendの双方向生成」を確認します。
`--verify-vpp`を実applyで指定すると、explicit routeのFIB存在確認に加えてinterface up確認を行います。

## 1. なぜscenario harnessを先に作るか

未踏提出向けには、単に実ネットワークを一回動かすだけではなく、次を示せることが重要です。

- 複数のpath候補を宣言的に定義できる。
- health/event条件を変えると選択pathが変わる。
- fallbackやrecoveryの判断理由を説明できる。
- 新しい経路選択アルゴリズムを差し替えて試せる。

本番daemonを急いで作ると、OSプロセス管理、権限、VICI/VPP API、ログ管理などに実装時間を取られます。

そこで先に `eventnet_scenario` を作り、controller判断部分を小さく再現可能に検証します。

## 2. eventnet_scenario の役割

`eventnet_scenario` は、YAML configに対してテスト条件をCLIから注入し、controllerがどのpathを選ぶかを確認します。

想定入力:

- YAML config
- `--mode explicit|priority|evaluated`
- `--active-path PATH`
- `--fail-path PATH`
- `--health PATH=state[,rtt=N,loss=N]`
- `--expect PATH`
- `--generate-runtime`

想定出力:

- selected path
- selection reason
- excluded pathと理由
- injected health
- expectation pass/fail
- runtime生成先

用途:

- path selection policyの単体検証。
- fallback/recovery条件の再現。
- evaluated selectionの比較条件テスト。
- 未踏デモ用の説明ログ生成。

## 3. Production eventnetd の役割

本番 `eventnetd` は、長時間動作するcontroller processです。

想定入力:

- YAML desired state
- strongSwan observed state
- VPP observed state
- Health probe result
- 外部API/CLIからのIntent update
- timer/event source

想定出力:

- applied state
- observed state
- transition state
- audit log
- explain/status API
- adapter操作

用途:

- 実ネットワーク状態を継続監視する。
- Desired / Observed / Applied の差分を検出する。
- 必要に応じて再適用、fallback、rollbackする。
- controller再起動後に状態を復元する。

## 4. 差分一覧

| 項目 | eventnet_scenario | production eventnetd |
|---|---|---|
| 実行形態 | 1回実行CLI | 常駐daemon |
| 状態入力 | CLI引数で注入 | adapter/health/eventから観測 |
| strongSwan | mockまたは生成runtime接続 | VICI/API/状態購読 |
| VPP | mockまたは生成runtime接続 | vppctl/API/状態取得 |
| Health | `--health` で注入 | ping/counter/BFD等で測定 |
| Event | `--fail-path` などで注入 | event queue / watcher |
| State store | process内一時状態 | 永続/復元可能な状態 |
| Reconcile | 1回の判断 | 継続的な収束処理 |
| Rollback | 選択・plan検証中心 | 実adapter操作の失敗から復旧 |
| Explain | stdout中心 | file/API/structured log |

## 5. 共通化するべきもの

`eventnet_scenario` で作ったもののうち、productionでも使うべきもの:

- YAML parser
- data model
- path selection
- transition state enum
- health state model
- explanation model
- runtime generationの一部
- tests/scenario definitions

特に `en_select_path` と `en_reconcile_result_t` は、scenarioとproductionの両方で中心になります。

## 6. 本番化までに追加が必要なもの

### 6.1 Daemon lifecycle

- foreground/background mode
- signal handling
- config reload
- pid/runtime directory
- root権限チェック
- systemd unit候補

### 6.2 Event source

- file watcher
- timer
- CLI/API intent update
- strongSwan VICI event
- VPP route/interface/counter observation
- health probe result

### 6.3 State store

- desired state
- observed state
- applied state
- transition state
- health state
- error state
- restart recovery metadata

### 6.4 Adapter observation

strongSwan:

- IKE SA一覧取得
- CHILD SA一覧取得
- SA event購読
- DPD/rekey/delete検知

VPP:

- interface状態取得
- route/FIB確認
- counter取得
- route apply結果確認

Health:

- end-to-end ping
- RTT/loss/jitter測定
- consecutive success/failure管理

### 6.5 Reconciliation loop

- desiredとobservedの差分検出
- path candidate再評価
- transition開始条件
- failure/recovery threshold、健全active Pathの時間ベースhold-down、品質差分hysteresis（実装済み）
- fallback/recovery policy
- transitionの有限retry/backoff、socket reconnect retry/backoff（実装済み）
- rollback

### 6.6 Apply orchestration

- prepare
- validate
- commit
- confirm
- rollback
- cleanup
- partial failure handling

### 6.7 Explain/status API

- selected path
- rejected candidates
- health snapshot
- active/applied path
- transition state
- last commands
- last error

初期はfile出力でよいです。

## 7. 破棄してよいscenario専用部分

productionへ持ち込まなくてよいもの:

- `--health` の手動注入parser
- `--expect` のpass/fail判定
- scenario専用step名
- stdout中心のテスト出力
- mock前提の一部ショートカット

ただし、これらはテスト用途として残す価値があります。

## 8. 実装順

具体的なファイル配置、関数名、出力先は `docs/future-implementation-map.md` にまとめています。

推奨順:

1. `eventnet_scenario --once` を作る。
2. `--health` / `--fail-path` / `--expect` を実装する。
3. evaluated selectionの実験を増やす。
4. `--generate-runtime` で既存 `eventnet_netns_plan` と接続する。
5. scenario smoke scriptを追加する。
6. event fileを読む簡易loopへ進む。
7. production `eventnetd` のstate store / event loopへ進む。

この順なら、実験可能性を早く示しつつ、本番実装への道筋も崩れません。
