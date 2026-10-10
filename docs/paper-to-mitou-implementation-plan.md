# 論文提出から未踏期間までの実装計画

2026-10-10更新: 論文前のC入出力・制御基盤とnamespace実疎通、評価専用executorの反復を完了した。Agent並列測定は論文後の実装予定で、有限parallel受信とは別。Nativeの過去CIと今回のGRE/Direct/Hub VM実測を区別する。

確認日、構成、測定制限の共通一覧: [現在の実装と検証状況](current-status.md)。

## 判断の前提

Ibukiの制御入力は、Web UIが生成する検証済みの正規スキーマを中心にする。YAMLは宣言的な設定の持ち運び、CI、再現実験、バックアップのために残す。したがって、論文提出時点でYAML 1.2全体を実装することは目的にしない。Controllerは入力形式にかかわらず、未知の項目・範囲外の値・未対応機能を拒否する。

CのJSONL処理は入力・出力ともyyjsonを使用する。Python評価器は標準JSONライブラリを使う。JSONLの1行区切り、Ibukiスキーマ、鮮度、重複、測定値の妥当性はController側で検証する。yyjson導入による速度比較は本計画の評価項目にしない。

## 論文提出まで

### 必須実装

1. yyjsonによるJSONL入力の実装を維持し、Agent、eventnetd、シナリオ、strongSwan Observer、VPP ObserverのJSON出力もyyjsonで生成する。
2. 現在の固定長配列モデルと上限値を文書化し、上限超過をエラーとして扱う。
3. YAMLはIbukiが定義する正規スキーマを対象に、経路選択、優先度、Explicit、Priority、Evaluated、waypoint、VLAN、VRF、FIB、fallbackを検証する。
4. Agentの逐次測定を基準実装とし、測定結果をControllerへ投入してPath選択へ反映できることを確認する。
5. direct障害、hub fallback、recovery、relay選択、rollback、XFRMポリシー、VLAN/FIB計画を再現可能なテストとして残す。
6. strongSwanをIPsec Backendとして利用し、Linux評価環境ではLinux XFRMによるIPsec SA、経路、疎通、rollbackを確認する。IbukiはIKE・暗号処理・OS固有のIPsecデータプレーンを再実装しない。
7. VPPはroute／VRF／VLAN／forwardingのBackendとして扱う。加えて、当初の必須範囲を越えてVPP GREとLinux XFRMをhandoffするnamespace v2実データパスまで検証した。
8. `gre_over_ipsec`のYAML・VPP GRE計画生成と、strongSwan/XFRM backendによるGRE経由疎通を実装済みとして扱う。VPP Native backendはIPIP + IPsecの比較用実験構成であり、GRE実装または本番用鍵管理の完成とは主張しない。

### 論文提出時点では実装しないもの

- YAML仕様全体への完全準拠。
- 履歴検索を目的とした外部Telemetry DB。
- VPP Binary APIの全メッセージcodec。
- 本番用のHA、BGP/FRRによる動的経路交換、証明書自動更新、Flow Preserve。
- OSPFなどマルチキャストを利用する動的経路制御。
- Linux GREを標準Backendとする実装。Linux GREは比較用の補助実験を除き、標準構成には採用しない。
- VPP Native IPsecのIKE連携、動的SA同期、鍵更新、本番向けsecret管理。静的SA・鍵によるIPIP/IPsec疎通は実験実装済みである。

## 論文提出から未踏期間まで

1. Web UI/APIの正規入力を定義し、UI入力をControllerの検証済み内部モデルへ変換する。Controllerでも同じ制約を再検証する。
2. Agentに任意Pathの測定周期、回数、timeout、並列数、送信元を指定できる設定を追加する。
3. 並列測定を実装する。最初はLinuxで上限付きワーカ数の実験モードとして導入し、逐次モードをフォールバックとして残す。1ラウンドの測定開始時刻を揃え、結果に測定ラウンドIDと開始・終了時刻を付ける。
4. 閾値、ヒステリシス、hold-down、recovery条件を設定ファイルから変更できることを確認し、切替回数と誤切替を測定する。
5. Telemetry保存の抽象インターフェースを追加する。論文提出時はJSONLファイルを標準実装とし、必要になった場合だけSQLite等を差し替えられる形にする。
6. VPP Binary APIとstrongSwan VICIを実機に接続するための最小adapterを完成させ、vppctl/CLIは診断用・互換用として残す。
7. strongSwanのOS固有Backend（Linux XFRM、BSD PF_KEY等）をCapabilityとadapter境界へ整理し、Linux固有の補助処理を本体から分離する。
8. VPP GRE＋strongSwan/XFRMは今回のVMで再確認し、VPP Native IPIP/IPsecは過去のCIで疎通確認済みである。同じcommit・環境・設定で両方式を再実行してから、切替時間、通信断、CPU、メモリ、MTU影響を比較する。

## 未踏期間中

1. 複数VM、物理機、クラウド拠点を含む実験環境を構築し、Direct、Hub、Relay、クラウドIPsecの切替を実測する。
2. Telemetry履歴保存は後段で必要性を確認して導入する。用途はダッシュボード、長期傾向、閾値調整、障害解析、イベント再生、複数Agentの時系列相関であり、Path選択に必須ではない。小規模構成はSQLite、時系列・多拠点構成はPrometheus等を候補とし、経路制御の実測を優先する。
3. 証明書認証、鍵更新、失効確認、認証情報の安全な格納をstrongSwan VICI操作と結合する。
4. VLAN/VRF/FIBをVPP Binary APIで実反映し、IPsec対象外通信の遮断とrollbackを実トラフィックで検証する。
5. 実験済みのVPP Native IPIP/IPsecを、IKE、SA同期、鍵更新、継続観測、rollbackを含む独立Backendへ発展させる。
6. 検証済みのVPP GRE + strongSwan/XFRMを反復測定し、必要ならVPP Native GRE + IPsecを別構成として追加する。Native IPIP/IPsecをGREと呼ばない。
7. FRRoutingを接続し、BGPによるprefix広告・withdrawalと経路収束を実測する。
8. OSPFを追加し、GREのマルチキャスト収容とVPP FIB反映を検証する。
9. GREとVTIを同じPath／Intent条件で比較し、MTU、経路収束、切替時間、障害観測粒度、CPU使用率を評価する。
10. クラウド能力プロファイル、Controller HA、再起動復旧を追加する。
11. Flow Preserveを、既存フローの識別、二重経路、切替中の状態保持、失敗時rollbackの順に実装する。
12. 並列測定を本番モードへ昇格し、CPU・ソケット・同時数の上限、過負荷時の抑制、測定失敗時の再試行を実装する。

## 一般YAML完全対応の扱い

一般YAML完全対応は不要である。Web UI/APIを主経路にする場合、重要なのはYAMLの文法網羅率ではなく、正規スキーマの互換性、エラー表示、再現性、安全な拒否である。ただし外部利用者がYAMLを直接編集する場合に備え、対応キー、型、既定値、上限、未対応キーの扱いを明示し、将来libyaml等へ置換できるParser境界を維持する。

## 並列測定の採否

並列測定は実装する。複数Pathを順番に測ると測定時刻がずれ、障害発生時の比較が不公平になるためである。一方、無制限並列はAgent自身が回線を圧迫するので、既定値は安全な小さい並列数、設定可能な最大値、timeout、キャンセル、逐次フォールバックを必須とする。並列測定は論文後に実装し、逐次基準との差は将来評価する。現在の論文では並列Agent実装・速度向上を主張せず、測定時刻の整合性と未実装範囲を明示する。

## 実装責務とファイル配置

| 責務 | 現在の場所 | 実装済み範囲 |
| --- | --- | --- |
| モデル・契約 | `include/eventnet/types.h`、`include/eventnet/controller.h` | 固定長モデル、Intent、adapter、結果 |
| 設定 | `src/yaml_config.c`、`include/eventnet/yaml_config.h` | 正規YAML、参照・上限・token・route・VLAN・能力検証 |
| reconcile・選択 | `src/controller.c`、`src/path_selection.c` | `en_controller_submit_intent`、3方式、fallback、閾値 |
| 遷移 | `src/transition.c` | prepare/ready/commit/confirm、retry、cleanup、rollback、簡易Graceful |
| 状態・説明 | `src/state.c`、`src/audit.c`、`src/json_output.c` | 適用Path・履歴、Cのyyjson出力 |
| 観測入力 | `src/telemetry.c`、`include/eventnet/telemetry.h` | JSONL、型・範囲・identity、入力上限 |
| daemon | `examples/eventnetd.c` | file/stdin/socket、有限parallel batch、state、reload、UID/rate制限、VICI monitor |
| Agent | `examples/eventnet_agent.c` | 逐次ping、候補展開、JSONL、連続成功/失敗 |
| command | `src/command_adapters.c`、`include/eventnet/command_adapters.h` | swanctl/VPP CLI、route/GRE/VLAN/FIB/ACL/XFRM、verify |
| VICI | `src/strongswan_vici_adapter.c`、`src/strongswan_vici_client.c` | 任意libviciビルド、操作・観測・購読・再接続 |
| VPP API | `src/vpp_api_adapter.c`、`src/vpp_api_transport.c` | callback・transport。版依存codecは未実装 |
| 計画 | `src/apply_plan.c`、`examples/netns_plan.c` | swanctl、node別VPP、apply/rollback |
| 実験 | `scripts/paper-runtime.py`、`scripts/vm-paper-collect-metrics.sh` | 実障害・復帰・部分Rollback・帯域・VLAN/FIB |

scenarioとeventnetdはすでに共通C ControllerへIntentを渡す。将来入力管理・status・計画生成を分離する場合の新ファイル名や関数名は未確定。

## 論文後から未踏開始まで

### Agentの上限付き並列測定

場所: `examples/eventnet_agent.c`、`src/telemetry.c`、公開Telemetryヘッダ、Agent回帰試験。

worker数、timeout、キャンセル、逐次fallbackを設定化し、ラウンドの開始/終了・測定主体・欠測を記録する。`eventnetd --socket-parallel`は有限同時受信でありAgent並列probeではない。最終segment endpoint測定は全segment品質の観測でもない。

### 常駐reconcileと安全な適用

場所: `examples/eventnetd.c`、`src/controller.c`、`src/transition.c`、`src/state.c`、`deploy/ibuki-eventnetd.service`。

無期限parallel接続、backpressure、入力/event/reload順序、再起動時のDesired/Observed/Applied再照合、外部経路変更検出を追加する。callback成功だけでなくcommit後実疎通をStable判定へ接続する。`en_now_ms`はepoch wall clockなので、経過時間保証にはTelemetry時刻と単調時計を分ける。

systemdのUID、socket、秘密情報、command権限を実環境で確認する。Python評価executorだけでは本番化は完成しない。

### VICIとVPP API

場所: VICI client/adapter、VPP API adapter/transport、公開ヘッダとprobe。

VICI rekey/DPD、切断・再起動、認証・鍵更新を運用検証する。VPP SDK版を固定しroute/VRF/VLAN/interface codec、timeout、戻り値、rollback、観測identityを実装する。受信FD・message送信境界だけではFIB更新の完成ではない。CLIは互換・診断用に残す。Native静的SAは動的SA同期と別物。

## 未踏期間中

| 領域 | 場所 | 受け入れ条件 |
| --- | --- | --- |
| 閾値・推奨値 | Agent/selection/YAML/評価器 | 多拠点の遅延/loss/jitter/帯域変更。ユーザ設定可能 |
| Graceful | transition/adapter | readiness、pause/drain、commit後疎通、失敗復旧 |
| VLAN/VRF/遮断 | command/API/生成器 | Intent実適用、一覧外・未タグ・IPv6・物理trunk |
| 証明書・Native SA | VICI/Native runtime | rekey、失効、鍵格納、双方向SA、観測・rollback |
| FRR/BGP/OSPF | 新規adapter/能力モデル | 広告/withdrawal、FIB、収束、GRE multicast |
| HA | state/reconcile/service | leader競合防止、Active維持、引継ぎ再照合 |
| Flow Preserve | transition/flow観測/forwarding | 既存/new flow分類・保持・二重経路・復旧 |
| Cloud/GUI/DB | 新規adapter/API/保存境界 | 必要性を確認して後段へ追加 |

capability/waypoint選択は実装済みだがCloud APIやFirewall/IDS自体は未実装。GRE/VTI比較、UDP、MTU、再鍵交換、長時間負荷は未測定。

## Namespace標準

標準は`vm-vpp-ns-runtime.sh`と`vm-vpp-ns-topology.sh`によるnode別VPP。Direct/Hub/GRE疎通を確認した。Hub IPsec中継はLinux/XFRM。root単一VPPの`vm-vpp-netns.sh`は互換・限定試験用。旧Hub制限を現行runtimeの未実装として扱わない。[Namespace Runtime v2](namespace-runtime-v2.md)を参照。

## 更新規則

新CLI・schema・関数はヘッダ、実装、回帰試験、[関数リファレンス](code-reference.md)、[Shell一覧](shell-commands.md)を同時更新する。将来案を現行実行例へ混ぜず、制御試験・機能疎通・定量評価・本番運用を分ける。
