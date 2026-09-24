# 未踏提出向け 実装到達点 詳細版

この文書は、PathWeaver の現時点の実装を、未踏提出・共同作業・デモ説明でそのまま使える粒度に整理したものです。

単に「どの機能があるか」を列挙するだけでなく、なぜその機能を実装したのか、どのような操作で何を確認できるのか、そして本番実装へ進む際に何が残っているのかを明確にします。

## 論文執筆へ移行する現在地（2026-09-25）

論文前のController基盤範囲は完了しています。root不要のCTest 27件がpassし、論文前validation成果物ではC単体、scenario、route YAML、Agent telemetry、閾値・安定性、イベント再選択、reload、status／plan security、Shell構文など12件がpass、root/VPP依存3件がskipです。`event-reconcile`ではdirect障害時のhub fallback、回復時のdirect復帰、VLAN route／interface観測、XFRM遮断、共有socket batch、state保存まで確認しています。

namespace v2では、strongSwan/XFRM backendのVPP GRE over IPsecと、比較用VPP Native IPIP/IPsec backendの双方について、双方向LAN疎通、暗号化・復号counter増加、停止後残留なし、再適用をGitHub Actionsで確認しました。成功実行はそれぞれ[36022032962](https://github.com/nagato-home-server/ibuki/actions/runs/36022032962)と[36022029474](https://github.com/nagato-home-server/ibuki/actions/runs/36022029474)です。Native構成はGREではなく、静的SA・鍵を用いたIPIP + `ipsec tunnel protect`であり、本番Backend完成とは扱いません。

ここからの論文提出上の必須作業は、同一条件の反復測定、切替工程別時間と通信影響・resource使用量の取得、提出対象commitへ紐付く統合成果物の保存、図表生成、本文とPDFの仕上げです。VPP NativeのIKE／鍵更新／SA同期、VPP Binary APIの版依存codec、strongSwanのrekey／DPD運用、FRR／BGP／OSPF、VTI比較、HA、Flow Preserve、実trunk分離、GUIは未踏期間以降の拡張とします。Gracefulは制御ロジック済みですが実runtimeの定量評価が残っています。

## 1. プロジェクト概要

PathWeaver は、strongSwan、VPP、将来的には FRRouting などの既存ネットワークOSSを、宣言的なIntentとイベント駆動の制御ロジックで束ねるためのネットワーク制御基盤です。

本プロジェクトの目的は、新しいVPNプロトコルや独自データプレーンを作ることではありません。IPsec、VPP forwarding、Linux network namespace など、既に存在する標準的な仕組みを利用し、その上位に「どの経路を選ぶか」「いつ切り替えるか」「なぜその経路を選んだか」を扱う制御プレーンを作ることを狙っています。

現在の実装では、YAMLで複数の候補経路を定義し、Cで実装したcontrollerがIntentとpolicyに基づいて経路を選択します。その選択結果から、strongSwan IPsec runtime と VPP forwarding runtime を操作するスクリプトを生成し、Linux VM上で実際に疎通確認できるところまで到達しています。

一文で言うと、現時点の PathWeaver は次のことができます。

```text
YAMLで宣言したIntent/Path/Policyを読み、
controllerが経路を選び、
strongSwanとVPPを組み合わせたruntimeを生成し、
障害・回復・品質条件の違いによる経路選択を実験し、
その判断理由をJSONLで説明できる。
```

## 2. 現在のプロトタイプで実証したいこと

未踏提出向けの現段階では、商用品レベルの常駐daemonやGUIではなく、「研究・実証用の制御基盤として価値があるか」を示すことを重視しています。

そのため、現時点の実証ポイントは次の4つです。

1. **宣言的なネットワーク制御**
   - 利用者は `swanctl` や `vppctl` の具体コマンドを書くのではなく、YAMLでIntent、Path、Tunnel、VPP edgeを定義します。
   - controllerはそのYAMLを読み、候補経路の集合と選択policyを理解します。

2. **複数経路の抽象化**
   - direct、hub、relayを特別扱いせず、すべて `Path` と `Segment` の組み合わせとして扱います。
   - これにより、将来的にSecurity GatewayやCloud POPなどの経由地を増やしても、同じモデルで扱える余地があります。

3. **イベント・品質条件による経路選択**
   - 通常時はdirectを選び、direct障害時はhubにfallbackし、direct回復時はpriorityに従ってdirectへ戻る、という流れをscenario harnessで再現できます。
   - RTTやpacket lossを注入し、evaluated selectionでrelayが選ばれるケースも再現できます。

4. **OSS runtimeとの接続**
   - strongSwanをLinux network namespace内で起動し、direct/hub IPsec tunnelを確立しています。
   - VPPをhost-interface経由でnamespaceへ接続し、controllerが生成したroute planを実VPPへ適用してLAN forwardingを確認しています。

5. **Node能力と運用状態の宣言**
   - YAMLの`nodes`でNodeのrole、endpoint、capability、administrative stateを表現できます。
   - Intentの`required_capabilities`で利用可能能力を経路選択条件にでき、disabled Nodeと未知Node参照を拒否・除外します。

## 3. 実装済みコンポーネント

### 3.1 C controller core

controller core は `src/` と `include/eventnet/` にあります。

主な役割は、YAMLから読み込んだモデルに対して、Intentを受け取り、Pathを選び、Transitionの骨格を実行し、結果を説明可能な形にまとめることです。

実装済みの主な要素:

- `Intent`
  - どの通信を、どのようなpolicyで制御するかを表します。
- `Path`
  - `site-a -> site-b` のdirectや、`site-a -> hub-1 -> site-b` のhub経由など、通信経路全体を表します。
- `Segment`
  - Pathを構成する1区間です。hub経由なら `site-a -> hub-1` と `hub-1 -> site-b` の2つのSegmentを持ちます。
- `Tunnel`
  - Segmentに対応するIPsec tunnelを表します。
- `VPP edge`
  - 各site nodeとVPP host-interface/next-hopの対応を表します。
- `Health`
  - Pathの状態、RTT、packet lossなどを表します。
- `Selection result`
  - 選ばれたPath、除外されたPath、選択理由を表します。

この段階では、controllerは本番daemonとして常駐するのではなく、CLIやscriptから呼び出して使います。これは実装を小さく保ち、選択ロジックやruntime生成を先に検証するためです。

### 3.2 YAML parser

YAML parser は `src/yaml_config.c` にあります。

現在読めるtop-level key:

- `tunnels`
- `vpp_edges`
- `paths`
- `intents`

例:

```yaml
vpp_edges:
  - node_id: site-a
    vpp_interface: host-vpp-site-a
    namespace_address: 172.16.1.2/30
    next_hop: 172.16.1.2
```

この情報により、Cコードに `172.16.x.x` のnext-hopを固定せず、YAML側でVPP接続点を定義できます。

YAML parserは本格的なYAMLライブラリではなく、現在の実証に必要なindentation-based parserです。将来的にYAML表現が複雑になる場合は、libyaml等への置き換え候補があります。

### 3.3 Path selection

Path selection は `src/path_selection.c` にあります。

実装済みの選択方式:

- `explicit`
  - 指定されたPathをそのまま選びます。
  - 実験や手動切替に向いています。
- `priority`
  - YAMLに書かれた候補順に、利用可能な最初のPathを選びます。
  - primary/secondaryの単純fallbackに向いています。
- `evaluated`
  - packet loss、latency、hop count、priority、path id などの比較順序に従って候補を比較します。
  - 品質に応じた経路制御の入口です。

evaluated selectionには、`failure_threshold` / `recovery_threshold`による連続判定、健全なactive Pathの`hold_down_ms`、品質差分に対する`hysteresis_percent`を実装済みです。単発の揺らぎによる頻繁な切替を抑えます。長時間負荷下での収束時間・切替頻度の実環境評価は今後の課題です。

### 3.4 Transition model

Transition model は `src/transition.c` にあります。

現在のcontroller内部では、次のような段階を持っています。

```text
prepare
-> validate
-> commit
-> confirm
-> completed
```

失敗時には、apply済みcommandに対応するrollbackだけを逆順で実行する骨格があります。対応情報がない手作成planには従来のrollback listを使う後方互換も残しています。

実runtime接続の中心は`swanctl`／`vppctl` adapterと生成shellですが、libviciの操作・観測・event購読とVPP Binary APIの低レベルtransportも接合境界まで実装しています。command adapterにはVICI socket指定、SA verify、VICI URIへのconnection config load、VPP CLI socket指定、VRF table・VLAN sub-interface準備、切替時のroute削除・旧Path再投入を実装しています。`block_non_ipsec`指定時はTunnel selector限定のXFRM blockも同じadapterから適用します。

### 3.5 Runtime generator

Runtime generator は `examples/netns_plan.c` です。

このCLIは、YAMLとcontrollerの選択結果から、Linux VM上で実行するruntime scriptを生成します。

生成される主なファイル:

- `out/netns-runtime/selected-path.txt`
  - controllerが選んだPath、Segment、Tunnel、VPP edge情報を出します。
- `out/netns-runtime/apply-selected.sh`
  - selected pathのIPsec runtimeだけを起動・検証します。
- `out/netns-runtime/apply-integrated.sh`
  - IPsec runtimeとVPP forwarding runtimeを一つの流れで連続制御します。
- `out/netns-runtime/vpp-route-plan.sh`
  - 一般的なVPP route planを出します。
- `out/netns-runtime/vpp-netns-route-plan.sh`
  - Linux VMのVPP host-interface構成向けroute planを出します。

このうち、提出デモで最も重要なのは `apply-integrated.sh` です。これは、controllerが選んだPathに応じて、strongSwan IPsecの起動・検証と、VPP route apply・forwarding smokeを一つの生成planで実行します。

### 3.6 Scenario harness

Scenario harness は `examples/eventnet_scenario.c` です。

本番daemon化前に、controller判断を高速に実験するためのCLIです。実ネットワークを毎回動かさず、healthやfailure eventをCLI引数で注入し、どのPathが選ばれるかを確認できます。

代表例:

```sh
build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub
```

この例では、現在のactive pathが `path-direct` で、そのdirectがfailedになった、という条件を注入します。その結果、YAMLのfallback policyに従って `path-via-hub` が選ばれることを確認します。

multi-step scenarioもあります。

```sh
build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --step direct-ok \
  --step direct-failed \
  --step direct-recovered \
  --step relay-best \
  --explain-json out/scenario/multistep-explain.jsonl
```

この1コマンドで、次の流れを順に確認します。

```text
direct正常
-> direct障害
-> hub fallback
-> direct回復
-> 品質条件でrelayが最良
```

`--explain-json` を指定すると、各stepの判断理由をJSON Lines形式で保存します。

### 3.7 Explain JSONL

Explain JSONL は、controllerの判断理由を機械可読に保存するための出力です。

例:

```json
{"schema":"eventnet.scenario.explain.v1","scenario_step":"direct-failed","selected_path":"path-via-hub","reason":"active path path-direct failed; using fallback path-via-hub","result":"pass"}
```

実際の出力には、次の情報も含まれます。

- YAML file
- Intent ID
- selection mode
- active path
- failed path
- selected path
- transition state
- reason
- expected path
- pass/fail result
- excluded paths
- injected health

この出力は、将来の `eventnetd` のstatus/explain APIの原型です。また、未踏提出時には「なぜそのPathを選んだか」を説明する材料になります。

## 4. 実ネットワークで確認済みのこと

### 4.1 direct IPsec

Linux network namespace上で、`site-a` と `site-b` の間にdirect IPsec tunnelを確立しました。

確認内容:

- `charon` を各namespace内で起動。
- `swanctl.conf` を生成・load。
- `tun-a-b` CHILD SAを確立。
- `site-a` から `site-b` へping。
- `swanctl --list-sas` のESP packet counterが増加することを確認。

単なるping疎通ではなく、ESP counter増加を見ているため、trafficがIPsecに乗っていることを確認しています。

### 4.2 hub IPsec

Linux network namespace上で、`site-a -> hub-1 -> site-b` のhub経由IPsec pathを確立しました。

確認内容:

- `site-a` / `hub-1` / `site-b` で `charon` を起動。
- `tun-a-hub` と `tun-hub-b` を確立。
- XFRM interfaceを使ってroute-based IPsecを構成。
- `site-a` から `site-b` へhub経由でping。
- `site-a/tun-a-hub` と `hub-1/tun-hub-b` のESP counterが増えることを確認。

hub pathでは複数のIPsec tunnelを扱うため、directよりも「PathがSegmentの集合である」という設計が見えやすくなっています。

### 4.3 VPP forwarding

VPPをLinux namespaceと接続し、VPPがL3 forwarding planeとして動作することを確認しました。

構成:

```text
site-a:vpp-client 172.16.1.2/30
  <-> VPP host-vpp-site-a 172.16.1.1/30

site-b:vpp-client 172.16.2.2/30
  <-> VPP host-vpp-site-b 172.16.2.1/30
```

確認内容:

- VPP host-interfaceを作成。
- namespace側vethとVPP host-interfaceを接続。
- controller生成のVPP routeを `DRY_RUN=0` で実適用。
- `site-a` / `site-b` のLAN routeをVPP edgeへ向ける。
- `10.10.1.1 <-> 10.10.2.1` のpingがVPP経由で成功。

### 4.4 Integrated runtime

`apply-integrated.sh` により、IPsec runtimeとVPP forwarding runtimeを同じcontroller-generated planで連続制御できます。

direct modeで確認した流れ:

```text
YAML/controller selects path-direct
-> generated apply-integrated.sh
-> start direct IPsec runtime
-> check ESP counters
-> setup VPP host-interface
-> apply VPP routes
-> verify VPP forwarding
```

fallback modeで確認した流れ:

```text
active path-direct failure event
-> controller selects path-via-hub
-> generated apply-integrated.sh
-> start hub IPsec runtime
-> check hub-path ESP counters
-> setup VPP host-interface
-> apply VPP routes
-> verify VPP forwarding
```

確認済みログ:

```text
Controller integrated runtime smoke passed: MODE=direct
Controller integrated runtime smoke passed: MODE=fallback
```

注意点として、現段階では「同じcontroller-generated planでIPsecとVPPを連続制御する」統合です。同一packetがIPsec復号後にVPP forwarding pipelineを連続通過する本番gateway pipelineは、次段階の設計・実装課題です。

## 5. 代表デモ

### 5.1 安全な非rootデモ

通常ユーザーで実行できる範囲のデモです。

```sh
sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

このデモで行うこと:

1. C controllerをbuildする。
2. C testを実行する。
3. scenario harnessでpriority/fallback/recovery/evaluated selectionを確認する。
4. Explain JSONLを表示する。
5. fallback条件でruntime planを生成する。
6. `selected-path.txt` と `apply-integrated.sh` の生成を確認する。

このデモはroot権限を必要としないため、審査や共同作業の場で安全に見せやすいです。

### 5.2 実IPsec/VPPを含むVMデモ

実際にstrongSwanとVPPを動かすデモです。

```sh
sudo RUN_RUNTIME=1 sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

このデモでは、非rootデモの内容に加えて、実IPsec/VPP runtime smokeを実行します。

必要な前提:

- Linux VM
- network namespace作成済み
- strongSwan / swanctl / charon
- VPP / vppctl
- root権限

個別に確認する場合:

```sh
sudo sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
sudo MODE=fallback sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
```

## 6. 出力として見せるべきもの

### 6.1 `out/scenario/multistep-explain.jsonl`

scenario stepごとの判断理由です。

見るべき点:

- `direct-ok` で `path-direct` が選ばれている。
- `direct-failed` で `path-via-hub` が選ばれている。
- `direct-recovered` で `path-direct` に戻っている。
- `relay-best` で injected health により `path-via-relay-c` が選ばれている。

### 6.2 `out/netns-runtime/selected-path.txt`

controllerが選んだPathの説明です。

見るべき点:

- `selected_path`
- `runtime_kind`
- source/destination
- route destination prefix
- VPP edge mapping
- Segment一覧
- Segmentに対応するTunnel情報

### 6.3 `out/netns-runtime/apply-integrated.sh`

controllerが生成した統合runtime scriptです。

見るべき点:

- directならdirect IPsec scriptを呼ぶ。
- hubならhub IPsec scriptを呼ぶ。
- その後VPP host-interface setupとVPP route applyを行う。
- 最後にVPP forwarding smokeを行う。

## 7. 現時点で「できている」と言えること

提出・説明では、次のように言えます。

```text
PathWeaverは、YAMLで定義された複数の拠点間経路候補に対して、
controllerがIntentとpolicyを評価し、
direct/hub/relayのいずれかを選択できる。

また、選択結果からstrongSwanとVPPのruntime planを生成し、
Linux VM上でdirect IPsec、hub IPsec、VPP forwarding、
およびIPsec+VPP統合runtime smokeを確認している。

さらに、scenario harnessにより、
障害・回復・品質条件をCLIで注入して、
controllerの判断と説明JSONLを再現できる。
```

## 8. 本番実装との差分

現時点の実装は、提出向けプロトタイプです。本番controllerとの差分は以下です。

### 8.1 常駐daemonではない

`eventnetd` はCLIとして実装済みで、周期評価、標準入力、Linux Unix socket入力、status JSONL、state file復元、file入力の`--reload-config`、再読込失敗時の旧設定維持、Linuxの`--reload-on-sighup`、UID認証、入力レート制限を持ちます。`--batch-size`により複数PathのAgent測定を1ラウンドとして反映でき、`--socket-parallel`では有限N接続をpollで同時受信して共有Controller状態へ統合します。`--socket-accept-count 0`では無期限の逐次受信も行えます。Linuxのstatus JSONL出力は`O_NOFOLLOW`付きdescriptorで開き、symlink・非regular file・group／other書き込み可能な既存ファイルを拒否します。`vm-evaluate.sh telemetry-long`ではAgentのJSONLをeventnetdのfile周期入力へ渡し、reconcile回数・status JSONL・state fileを一括評価できます。`telemetry-live`ではAgentが1件ずつ追記する間にeventnetdが同じファイルを周期再読込します。failure/recoveryイベントの入力とstale telemetryの期限判定もsmokeで検証しています。systemd unitテンプレートは追加済みですが、実環境での権限・socket整合性検証と無期限parallel service化は未実施です。

既定のstrongSwan/VPP command rendererは、YAML由来の識別子・アドレス・route値を許可文字検証してからshell commandへ展開します。YAML loaderは固定長fieldの容量超過も拒否し、識別子の静かな切り詰めによる衝突を防ぎます。管理者が指定する任意template commandは実行権限を持つため、提出後の本番化ではexec引数配列またはVICI/VPP APIへ移行します。
Linuxの既定command adapterは引数を分割して`execvp`で実行し、shellを介しません。任意templateもshellメタ文字を含まない単純commandは同じexec経路へ送り、パイプ・リダイレクトを明示したtemplateだけを後方互換のshell拡張として残します。WindowsではCLI互換のsystem fallbackを使います。

### 8.2 状態観測は限定的

Agentのping telemetryと期限判定は実装済みです。strongSwanはcommand実行・SA verify・libviciの有限／無期限event probeまで、VPPはCLI observerとBinary API transportの接続・受信FD・generic event callback、生成messageのallocate／send／free／availability境界まで実装しています。VPP生成messageによるFIB操作や、各backendの継続counter観測は未実装です。
AgentはPath IDとsource labelを検証してからJSONLへ出力し、入力値による壊れたrecord生成を防ぎます。
YAMLから候補Pathを展開する場合は、直列Pathの最後のsegmentに対応するremote endpointを測定し、segmentを持たないlegacy routeでは`route_next_hop`へfallbackします。
また、`ibuki.event.path.v1` の `path_failed` / `path_recovered` をJSONLから受ける最小イベント境界を実装しています。VICIの有限／無期限probe監視と、libvici有効ビルドのeventnetd `--vici-monitor-*`は同じtelemetry境界へ接続できます。
strongSwan tunnel状態とVPP route状態についても、`ibuki.event.tunnel.v1`／`ibuki.event.vpp.route.v1`を同じhealth入力へ変換できます。VPP interface状態は`eventnet_vpp_interface_observer`で`ibuki.event.vpp.interface.v1`へ変換でき、VLAN sub-interfaceのidentityとup/downをeventnetdで検証します。実VICIについてはeventnetd内蔵monitorからこのschemaを継続生成でき、VPP API側は生成messageを専用adapterから同じschemaへ変換する段階です。
ファイル入力とLinux Unix socket入力の両方で、同一接続から複数イベントをbatch単位で連続reconcileするsmokeを用意しています。
Unix socketは起動時に所有者限定権限で作成し、指定パスに通常ファイルが存在する場合は上書き・削除せず停止します。
state fileは一時ファイルへ書き込み完了後に置換し、reconcile途中のプロセス終了で前回の復旧情報が空になるリスクを抑えます。復元時は現在のIntentから再構成したtraffic key、Path所属、Pathと全経由Nodeのadministrative stateと照合し、別Intent・候補外・無効化済みPath／Nodeのstateはfail-closedで拒否します。
VPP observerはVRF付き`show ip fib`について`--table TABLE_ID`で対象tableを選択でき、`table_id`付きroute eventをeventnetdへ入力する評価まで実装しています。不正なtable IDはCLIとtelemetry parserの両方で拒否します。

### 8.3 strongSwan VICI event購読は部分実装

現時点では `swanctl --uri` とscriptでSA確立・`--list-sas --child`確認を行っています。`en_strongswan_parse_list_sas`により、`swanctl --list-sas`のchild stateをObserved Tunnelのstate/healthへ変換し、Linuxの`--verify-swanctl`実行時には取得出力をパーサへ渡して`INSTALLED`／`REKEYING`以外を切替失敗として扱います。加えて、libviciがある環境では`eventnet_strongswan_vici_probe`で実socketへのversion request、指定CHILDのinitiate／terminate、SA観測、有限時間の`child-updown` event購読、切断後の有限回再接続、`monitor-forever`による無期限event配信を確認できます。`eventnet_strongswan_vici_controller_probe --monitor`とeventnetdの`--vici-monitor-*`では、child-updown eventをtelemetryとして同じcontrollerへ再reconcileできます。未実装なのは、rekey／DPD等のイベント種別に応じた個別運用ポリシーと、service再起動時の購読復旧固定です。

### 8.4 VPP binary API連携は一部実装

現時点では主に `vppctl` commandを生成・実行しています。`--verify-vpp`を指定したcommand backendでは、route apply後に`show ip fib`を取得し、期待prefixごとの存在・期待next-hop一致を確認します。explicit routeにinterfaceがある場合はさらに`show interface`でup状態を確認し、VLAN IntentではYAMLの全VPP edgeについて`vpp_interface.VLAN_ID`のup状態も確認します。本番ではVPP API、counter observationなどをさらにadapter化する必要があります。
VPP Binary APIについては、CMakeの`EVENTNET_ENABLE_VPP_API`オプションとLinux preflightで`vapi/vapi.h`／`libvapi.so`、`libvapiclient.so`、または`libvppapiclient.so`の有無を検出できます。API未導入環境では従来のCLI adapterを使用し、依存が揃った環境でのみ次段階のAPI adapterを有効化します。
また、VAPI生成コードをcallbackとして注入する`en_vpp_api_adapter`境界を追加しました。controllerのPath遷移とVPP SDKのmessage生成を分離し、SDKの版差をcontroller本体へ漏らさない構成です。
さらに、`en_vpp_api_apply_path_operations`でPathをroute／VRF／VLANの操作順へ正規化し、生成message callbackへ渡す境界を追加しました。実SDKのmessage codecとVPP FIBへの実反映はLinux SDK確認後の残課題です。
interface観測callbackとobserver event schemaも同じ境界へ追加し、VLAN sub-interfaceの状態をroute選択前に検証できるようにしています。VLAN付きIntentではroute観測と対象sub-interface観測を同一Pathへ集約し、両方のupが揃わない場合はhealthyにしません。`deny_unmatched_vlan: true`では、VPP親interfaceへのIPv4/IPv6 deny-all ACL生成とcommand backend適用も行います。

VLAN Policyについては、IntentのVLAN IDとrequired waypointを選択結果・selected-path summaryへ保存し、VPP netns planにsub-interface作成・有効化を生成するところまで実装しています。VLAN IDごとのtraffic key分離と、Agent thresholdによるPath維持／fallbackも追加しました。`eventnetd --backend command`でもVPP edgeのsub-interfaceを存在確認し、不足時に作成・有効化してからrouteを投入します。さらに`block_non_ipsec: true`ではTunnel selector限定の双方向XFRM block／rollback planを生成し、常駐command backendのapply経路にも同じblock追加・削除を反映します。`deny_unmatched_vlan: true`ではVPP親interfaceへIPv4/IPv6 deny-all ACLを適用できます。`vpp_edges.allowed_vlans`ではedge単位の一覧外VLANをplan生成・command backendの両方で拒否します。VLANごとのFIB table割当もplan生成とcommand backendへ反映済みです。現行のVPP VLAN smokeではタグ付きLANの実疎通を確認できますが、一覧外VLANの実trunk評価とVPP上の実パケット分離は未検証です。
また、VLAN指定時の明示routeは `parent.VLAN` sub-interfaceを出力interfaceとして参照します。VPP edgeを含む専用サンプルで生成結果を検証していますが、実VPP上でのタグ付きLAN疎通はLinux VMで別途確認が必要です。
`deny_unmatched_vlan: true`を指定した場合は、VPP親interfaceへのIPv4/IPv6 deny-all ACL生成とcommand backend適用まで実装済みです。`allowed_vlans`による一覧外VLANの生成時拒否と、VLANごとのFIB table設定も実装済みです。未タグdropの実パケット評価、実trunkの複数VLAN評価、VPP上の実パケット分離は未検証です。
VPP edgeを含む経路はVPP-only runtimeとして生成でき、選択・統合apply scriptからVPP netns planへ接続します。IPsecとVPPを同一applyで組み合わせる運用は、引き続きLinux VMでの実測が必要です。

### 8.5 Graceful Transitionの範囲

Gracefulでは、旧Pathをdraining状態にして短いpause/drain期間を設け、新Pathへのforwarding切替後に旧Pathのrouteと専用tunnelを撤去します。切替失敗時は既存rollbackへ戻ります。TCPフローの識別・保持を行うFlow Preserveや、Gracefulの通信影響を定量測定する評価は未実装です。

### 8.6 GRE over IPsecとVPP Native比較Backendの現在地

strongSwan/XFRM backendでは、VPP GRE packetをLinux XFRMへhandoffする実データパスをnamespace v2で検証済みです。VPPがGRE outer packetを生成し、Linux XFRMがESPで暗号化し、対向側で復号後にVPP GREへ戻す構成で、双方向LAN pingとESP送受信counterの増加を確認しました。Linux GREは標準Backendにせず、GRE操作はVPPが担当します。

比較用のVPP Native backendも、IPIP + `ipsec tunnel protect`、静的SA・鍵により双方向疎通と両siteの`esp4-encrypt-tun`／`esp4-decrypt-tun` counter増加を確認済みです。この結果はNative IPsecの最小データパス成立を示しますが、GREとの組合せ、IKE、鍵更新、SA同期、secret管理を含みません。BGP／OSPF、VPP Binary APIによる実操作、VTI比較とともに本番化課題として残します。

### 8.7 実データパス検証の現在地（2026-09-25）

Archのローカル環境にはVPP／strongSwan runtimeを常設せず、Ubuntu上のGitHub Actions workflowを再現可能な検証環境として使用しました。strongSwan/XFRM GREは実行36022032962、VPP Native IPIP/IPsecは実行36022029474で成功しています。両workflowは双方向ping、暗号counter、cleanup、再適用、停止後残留を検査します。ローカルでVPPをコンパイルすることは、この機能確認の必須条件ではありません。

### 8.8 FRRoutingは未踏期間に実装

論文前のGRE over IPsec初期実装では、静的L3 routeを使用します。未踏期間にVPP GRE over IPsecへFRRoutingを接続し、まずBGPのprefix広告・withdrawalと経路収束を実測します。その後、OSPFのマルチキャスト収容とBFDを追加し、GREとVTIを同じPath Modelで比較します。

### 8.9 Flow Preserveは未完成

Flow Preserveの概念は設計にありますが、実際のflow分類、既存flow維持、新規flow割当制御はまだ実装していません。

### 8.10 同一packetのIPsec→VPP pipelineは実験検証済み、本番運用は未完成

namespace v2では、同一packetがVPP GREとLinux XFRMを連続して通る構成、およびVPP内でIPIPとNative IPsecを連続して通る構成を検証済みです。一方、動的鍵交換・更新、継続監視、障害時の自動収束、secret管理、長時間負荷を含む実運用gatewayとしての完成は今後の課題です。

### 8.11 GUIは未実装

GUIは後段です。現時点ではCLI、script、JSONL、text outputで実証しています。

詳細は `docs/scenario-vs-production.md` を参照してください。

## 9. 次に実装する候補

具体的な実装場所、関数案、出力ファイル案は `docs/future-implementation-map.md` に分けて整理しています。

未踏提出までに追加すると効果が大きい順:

1. **Linux VMでの再現評価固定**
   - `route-yaml`、`cert-auth`、event socket、state復元、VLAN tagged trafficを一つの評価手順へまとめる。
   - pass / partial / skipの根拠と実行時間を保存する。`vm-evaluate.sh`は実行環境manifest（時刻、OS/kernel、git revision、dirty状態、使用バイナリ）も出力する。

2. **VLAN／VRF／FIBの実反映と遮断評価**
   - VLAN tagged traffic、explicit route、XFRM block、VLAN対象外dropの差分をLinux VMで確認する。

3. **実transport接合**
   - strongSwan VICIはprobeからeventnetd stdinへの有限接続とeventnetd自身の有限／無期限購読まで実装済み。次にrekey／DPD運用とVPP Binary API messageを依存環境で実装する。
   - 既存のcallback adapterとobserver schemaをtransportから利用する。

4. **提出資料用の図**
   - YAML -> Controller -> strongSwan/VPP -> Explain JSONL の流れを1枚にする。
   - direct/fallback/evaluated の切替図を作る。

5. **IPsec/VPP同一packet pipelineの設計メモ**
   - 現runtimeとの違いを明確にする。
   - XFRM interface、VPP interface、Linux routeの関係を整理する。

6. **Agent／eventnetd長時間評価**
   - parallel socket、stale／sequence、state復元、再接続backoffを長時間測定する。

7. **scenario case追加**
   - required waypoint
   - forbidden waypoint
   - max RTT violation
   - packet loss violation

後回しでよいもの:

- GUI
- FRRouting本統合（VPP GRE BackendへのBGP／OSPF接続）
- VPP binary API
- strongSwan VICI event購読
- systemd unitの実環境検証（テンプレートは追加済み）
- 本番HA/永続DB
