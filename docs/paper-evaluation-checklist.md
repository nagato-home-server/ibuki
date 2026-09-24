# Paper Evaluation Checklist

この文書は、Ibukiの論文発表前評価を同じ順序で再現するためのチェックリストです。WindowsではC実装・設定・生成物を確認し、Linux VMではstrongSwan／VPPを含む実runtimeを確認します。

## 1. 共通準備

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CTestではcontroller単体に加えて、direct選択、direct障害時fallback、YAML route検証、Agent simulated telemetryを実行します。これにより、Linux VMを使わない環境でもPath選択とtelemetry schemaの回帰を確認できます。

Linux VMでは、必要な権限と依存を確認した後、namespaceを準備します。

```sh
sh scripts/vm-build-cc.sh
sh scripts/vm-check.sh
sudo sh scripts/vm-netns-setup.sh
```

VPP SDKを標準外prefixへ導入した場合は、`VPP_PREFIX`を指定してpreflightとbuildを実行します。

```sh
VPP_PREFIX=/opt/vpp sh scripts/vm-vpp-api-preflight.sh
VPP_PREFIX=/opt/vpp EVENTNET_ENABLE_VPP_API=ON sh scripts/vm-build.sh
```

各評価では`OUT_DIR`を指定し、`summary.md`、`summary.csv`、ログ、生成planを保存します。評価開始時のgit revision、OS、kernel、使用binary、評価YAMLと主要binaryのSHA-256、HEADとの差分SHA-256、VPP／libvici optional feature状態は`vm-evaluate.sh`が`environment.txt`へ記録します。
`summary.md`末尾のTotalsには、実装済みのpass、依存環境や未完成機能のpartial／skip、失敗したfailを分けて集計します。

## 2. 制御ロジック

| 評価 | 実行 | 合格条件 |
| --- | --- | --- |
| Path選択 | `sh scripts/vm-evaluate.sh scenario samples/linux-vm-netns.yaml` | priority、failure fallback、evaluated、multi-step recoveryがpass |
| Agent telemetry | `sh scripts/vm-evaluate.sh telemetry samples/linux-vm-netns.yaml` | JSONL、stdin、socket、freshness、statusがpass |
| Status output security | `sh scripts/vm-eventnetd-status-security-smoke.sh samples/linux-vm-netns.yaml` | status JSONLの通常出力、symlink拒否、危険権限拒否がpass |
| 周期評価 | `LONG_COUNT=10 sh scripts/vm-evaluate.sh telemetry-long samples/linux-vm-netns.yaml` | 指定回数のreconcileとstate更新がpass |
| threshold／hysteresis | `sh scripts/vm-evaluate.sh threshold samples/linux-vm-netns.yaml` | failure/recovery thresholdと切替抑制がpass |
| YAML route網羅 | `sh scripts/vm-evaluate.sh route-yaml samples/route-examples.yaml` | legacy、explicit、双方向、hub、relay、VLAN、invalid、segmentless境界がpass |
| VLAN VRF分離 | `ctest --test-dir build -R eventnet_yaml_vlan_vrf_isolation --output-on-failure` | VLAN 100/200の生成planがtable 100/200へ分離され、相互混線しない |
| VLAN table競合拒否 | `ctest --test-dir build -R eventnet_yaml_vlan_vrf_conflict --output-on-failure` | 同一nodeにtable 0/100を混在させた不正Pathがplan生成で拒否される |
| Hub waypoint VLAN | `ctest --test-dir build -R eventnet_yaml_vlan_hub_waypoint --output-on-failure` | source／hub waypoint／destinationの全edgeへsub-interface、ACL、table routeが生成される |
| Multi-port YAML validation | `ctest --test-dir build -R eventnet_yaml_multiport_vpp_edges --output-on-failure` | Hubの複数VPP edgeと明示interface選択が入力検証を通過する |
| Relay telemetry endpoint | `ctest --test-dir build -R eventnet_agent_yaml_relay_terminal_simulated --output-on-failure` | AgentがRelay経路の最後のTunnel endpointを測定対象にする |
| Legacy telemetry fallback | `ctest --test-dir build -R eventnet_agent_yaml_legacy_no_segment_simulated --output-on-failure` | segmentなしrouteが`route_next_hop`を測定対象にする |
| Legacy runtime plan | `ctest --test-dir build -R eventnet_yaml_legacy_no_segment_plan --output-on-failure` | segmentなしrouteが宛先routeのみのruntime planへ変換される |
| Legacy eventnetd replay | `ctest --test-dir build -R eventnet_file_replay_legacy_no_segment --output-on-failure` | segmentなしrouteのtelemetryをeventnetdが受理し、Pathを選択する |
| Ambiguous legacy runtime rejection | `ctest --test-dir build -R eventnet_yaml_explicit_no_segment_rejected --output-on-failure` | segmentなしで`routes`を明示したPathをruntime適用前に拒否する |
| Empty segmentless path rejection | `ctest --test-dir build -R eventnet_yaml_empty_no_segment_rejected --output-on-failure` | route情報のないsegmentなしPathをYAML検証で拒否する |
| Node capability | `sh scripts/vm-evaluate.sh node-capability samples/node-capabilities.yaml` | capability選択、disabled Node、未知Node検証がpass |
| Backend adapter再利用 | `sh scripts/vm-evaluate.sh backend-reuse samples/linux-vm-netns.yaml` | strongSwan／VPP公開adapter契約、共通Path選択、swanctl／VPP plan生成がpass |
| VPP GRE plan regression | `ctest --test-dir build -R eventnet_gre_over_ipsec_plan --output-on-failure` | VPP GRE／strongSwan計画生成が壊れていないことを確認する。実データパスはnamespace v2で別に確認する |

## 3. Runtime接合

```sh
sudo sh scripts/vm-netns-ipsec-direct-smoke.sh
sudo sh scripts/vm-vpp-controller-netns-smoke.sh
sudo MODE=direct sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
sudo MODE=fallback sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
```

合格条件は、LAN ping成功だけでなく、directではESP counter増加、fallbackではcontrollerが生成したHub planの適用、VPPでは生成routeによるforwarding成功です。

strongSwan/XFRM backendのGRE over IPsecは、次の順序を一括評価します。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

```text
VPP GRE interface生成
  -> strongSwan IPsec SA確立
  -> GRE内側IP／静的route適用
  -> GRE経由のL3疎通
  -> route切替とRollback
```

この評価では、GREをGRETAPやVXLANのようなL2延伸方式として扱いません。Linux GREは標準Backendにせず、VPPによるGRE操作を対象にします。FRRoutingによるBGP／OSPFの動的経路交換は未踏期間の評価対象です。

比較用のVPP Native backendはGREを使わず、IPIP + `ipsec tunnel protect`を使用します。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-native.yaml
```

両backendの合格条件は、双方向LAN ping、暗号化・復号counterの増加、停止後の残留なし、再適用後の再疎通です。Native構成は静的SA・鍵による実験用構成であり、IKE、鍵更新、SA同期を含む本番Backendとは区別します。

VICIが利用できる場合は追加で実行します。

```sh
cmake -S . -B build-vici -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON
cmake --build build-vici
sudo RUN_CONTROLLER_PROBE=1 VICI_BUILD_DIR=build-vici \
  sh scripts/vm-vici-eventnetd-smoke.sh
```

この評価で`eventnet_strongswan_vici_controller_probe`が成功し、VICI eventがeventnetdへ到達することを確認します。VPPはこのprobeではmockであり、VPP forwardingは上記の統合runtimeで別に評価します。

## 4. VLAN／セキュリティ

```sh
sudo VLAN_MATRIX="1 100 4094" sh scripts/vm-vpp-vlan-netns-smoke.sh
sh scripts/vm-evaluate.sh xfrm-policy samples/linux-vm-netns.yaml
sudo sh scripts/vm-evaluate.sh xfrm-cleartext samples/linux-vm-netns.yaml
```

確認項目:

- 許可VLANのsub-interface、route、ACLが生成・適用される。
- `allowed_vlans`にないVLANのplan投入が拒否される。
- `deny_unmatched_vlan`で未タグ通信を遮断できる。
- `block_non_ipsec`でTunnel selector外のcleartext fallbackを遮断できる。
- direct IPsecの暗号化通信ではESP counterが増加する。

実trunk上の複数VLAN分離とVLAN間分離は、別途NIC／switch構成を用意した追加評価です。

state復元の入力境界は、通常のCTestに加えて次で確認します。

```sh
ctest --test-dir build --output-on-failure -R 'eventnet_state_wrong_(intent|path)'
```

`state-wrong-intent.tsv`はtraffic key不一致、`state-wrong-path.tsv`はexplicit Intentの候補外Path、`state-disabled-node.tsv`はdisabled Node経由Pathを表し、どれも成功扱いにならないことを確認します。

## 5. 評価レポートの解釈

- `pass`: 指定環境で実装と期待結果を確認した。
- `skip`: 依存（libvici、VPP、root、systemd等）がないため未実行。成功とは解釈しない。
- `partial`: controller境界またはplan生成は確認したが、実backend全体は未確認。
- `fail`: 実装または評価手順に問題がある。ログを保存し、原因修正後に再実行する。

## 6. 発表時に明示する未完了範囲

- eventnetdプロセス内部の無期限VICI購読はCLIとして実装済み。Linux VMで`DURATION_MS=0`の停止・再起動・権限運用を確認する。
- VPP Binary APIのroute／VLAN／VRF実message codecと本番接続。
- FRR／BGP／OSPFによる動的経路交換、クラウド固有VPN API、複数Controller federation。
- Flow Preserve戦略と長時間高負荷評価。
- 実trunk上のVLAN間分離。

動的経路交換、VTI比較、クラウド固有VPN API、複数Controller federation、Native backendのIKE・鍵更新・SA同期はcontrollerのPath選択・telemetry・runtime plan生成、および今回の静的runtime疎通結果と混同せず、未踏期間の実装・評価として報告します。

## 7. 論文執筆へ移る判定

提出前に必要な成果物の一覧とPythonによる図生成方法は、`docs/paper-submission-minimum.md`に集約する。評価CSVから図を生成する例は次のとおりである。

```sh
python3 scripts/generate-paper-graphs.py \
  --summary-csv out/evaluation/YYYYMMDD-HHMMSS/summary.csv \
  --metrics-csv out/paper-metrics.csv \
  --out-dir out/paper-figures
```

root不要の論文前validationは、次の一括実行で全caseがpassすることを基準にします。

```sh
BUILD_DIR=build-paper-baseline sh scripts/vm-paper-validation.sh samples/linux-vm-netns.yaml
```

root不要範囲で確認する項目は、C単体、全Path選択方式、route YAML網羅、Agent／telemetry、閾値・安定性、event reconcile、socket、reload、status／plan security、Shell構文です。2026-09-25時点でCTest 27件は全件passしています。`out/paper-baseline-3/summary.csv`には論文前validationのroot不要12件がpass、root/VPP依存3件がskipとして残っています。

rootが必要な次の項目は、Linux VMの依存とsudoが利用できるときだけ追加実行します。加えてnamespace v2 workflowでstrongSwan/XFRM GREとVPP Native IPsec/IPIPを確認します。2026-09-25のGitHub Actionsでは両backendとも双方向疎通、ESP counter、再適用、停止後残留確認に成功していますが、提出用には同一commitのvalidation成果物へ統合して保存します。

```sh
sudo BUILD_DIR=build-paper-baseline RUN_RUNTIME=1 sh scripts/vm-paper-validation.sh samples/linux-vm-netns.yaml
```

VPP未導入は`skip`、charonがVICI socketを生成しない場合はruntime環境の`fail`として、controller coreの完了判定とは分けて記録します。これらの追加評価が未完了でも、論文では「実装済みの制御層」と「未検証または未実装の実backend運用」を明確に分離して記述します。
