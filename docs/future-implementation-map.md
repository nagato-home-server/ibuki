# Future Implementation Map

この文書は、今後どこに何を実装するかを迷わないための作業地図です。

現時点の実装は、YAMLからIntent / Path / Tunnel / VPP edgeを読み、controllerがPathを選び、strongSwan / VPP向けruntime scriptを生成し、Linux VM上でdirect / hub fallback / VPP forwarding / integrated runtime smokeまで確認できています。

Node一覧もYAMLで定義でき、role・endpoint・capability・administrative stateをPath選択へ反映します。Intentの`required_capabilities`はPathの全端点で照合され、disabled Nodeや未知Node参照はそれぞれ選択除外・YAML検証エラーになります。これはCloud VPN／FRR／VPPなどの能力差を、backend固有実装から分離して表現するための基礎です。`vpp_edges`は従来のNodeごとに1 edgeという後方互換形式に加え、`port_id`を持つ複数edgeを同一Nodeへ定義でき、明示routeのinterface指定でportを選べます。

scenario harnessで手動実験していたイベント注入は、`eventnetd --once`、周期実行、標準入力、Linux Unix socket、observer CLI経由のJSONL入力まで実装済みです。複数接続の逐次処理、有限parallel shared batch、state file復元、file入力のconfig reload、UID認証、入力レート制限も実装済みです。次は、無期限parallelの共有受信、service hardening、VICI／VPPの本番運用復旧を段階的に進めます。

## 1. 最優先で実装する場所

### 1.1 `eventnetd`運用入力の拡張

実装済みの接合境界:

- `examples/eventnetd.c`
- `scripts/vm-eventnet-event-smoke.sh`
- `examples/swanctl_observer.c`
- `examples/vpp_observer.c`

残っている拡張:

- 無期限parallel接続の共有受信と、共通reconcile loopのライブラリ分離
- systemd等のservice manager環境での権限・socket・ログ検証
- グループ／証明書等による接続元認証、無期限parallel接続時の共有controller状態

入力レート制限は`--max-records-per-second`、Linux peer UID認証は`--socket-uid`として実装済み。有限parallel接続も`--socket-parallel`で実装済み。グループ／証明書認証と無期限parallel時の共有状態は未実装のまま残る。

同一Pathに対するtelemetryはtimestampの単調性を確認し、遅れて到着した古いレコードをmock入力境界で破棄します。これは現在の単一eventnetdプロセス内での再送保護であり、複数プロセス・複数Agent間の時刻同期や永続的なsequence管理は別途必要です。

state fileはLinuxで排他的・symlink追従なしの一時ファイルへ書き込み、renameで置換する。復元時は現在Intentのtraffic keyとPath所属を照合する。daemonの権限分離と保存先ディレクトリの所有者検証は運用時に追加する。

現在の目的:

- YAML configを読む。
- event fileを読む。
- controllerにhealth / failure / active pathを反映する。
- Pathを再選択する。
- 必要なら既存runtime generatorにつなぐ。
- `out/eventnetd/status.jsonl` に判断結果を残す。

observerのCLI出力をJSONLへ変換する処理と、eventnetdのreconcile処理は分離している。実VICI購読やVPP Binary APIはこの境界へ接続する。

file入力の周期再読込は`--reload-config`として実装済み、再読込失敗時は直前の正常設定を維持して処理を継続します。Linuxでは`--reload-on-sighup`によりSIGHUP受信ごとの安全な再読込も実装済みで、`vm-eventnet-sighup-smoke.sh`で正常reloadと不正設定保持を再現できます。systemd unitテンプレートは追加済みで、実環境への権限・socket整合性確認は未実施です。

最初のCLI案:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --once \
  --events out/eventnetd/events.txt \
  --status-json out/eventnetd/status.jsonl \
  --generate-runtime
```

最初のevent file案:

```text
active_path path-direct
path_failed path-direct
health path-via-hub healthy rtt=12 loss=0.0
health path-via-relay-c healthy rtt=30 loss=0.1
```

期待する最小出力:

```json
{"intent":"intent-a-b","selected_path":"path-via-hub","transition_state":"completed","reason":"active path path-direct failed; using fallback path-via-hub"}
```

## 2. event parserを置く場所

最初は `examples/eventnetd.c` 内に小さく実装してよいです。

ただし、行数が増えたら以下へ分離します。

- `include/eventnet/event_file.h`
- `src/event_file.c`
- `tests/test_event_file.c`

想定する関数:

```c
int en_event_file_load(const char *path, en_event_batch_t *batch, en_error_t *error);
void en_event_batch_init(en_event_batch_t *batch);
void en_event_batch_free(en_event_batch_t *batch);
```

扱うevent種別:

- `active_path <path_id>`
- `path_failed <path_id>`
- `path_recovered <path_id>`
- `health <path_id> <state> rtt=<ms> loss=<percent>`

この層は、将来strongSwan / VPP / health probe adapterから来るObserved Stateの代替入力です。

## 3. reconcile coreを置く場所

現在、scenario実験用のevent注入と選択処理は主に `examples/eventnet_scenario.c` にあります。

`eventnetd` でも同じ判断を使うため、次段階で以下へ切り出します。

- `include/eventnet/reconcile.h`
- `src/reconcile.c`
- `tests/test_reconcile.c`

想定する関数:

```c
int en_reconcile_once(
    en_controller_t *controller,
    const en_intent_t *intent,
    const en_event_batch_t *events,
    en_reconcile_result_t *result,
    en_error_t *error);
```

`en_reconcile_once()` が担うこと:

- event batchをcontroller stateへ反映する。
- active pathのfailureを解釈する。
- `src/path_selection.c` の選択器を呼ぶ。
- transition stateを更新する。
- explain outputに渡す結果を作る。

既存で読むべき場所:

- `src/controller.c`
- `src/path_selection.c`
- `src/transition.c`
- `include/eventnet/controller.h`
- `include/eventnet/path_selection.h`
- `include/eventnet/transition.h`

## 4. status / explain出力を置く場所

現在、JSON Lines出力は `examples/eventnet_scenario.c` 側にあります。

将来はscenario harnessと `eventnetd` の両方で使うため、以下に切り出します。

- `include/eventnet/explain_json.h`
- `src/explain_json.c`
- `tests/test_explain_json.c`

想定する関数:

```c
int en_explain_json_write_line(
    FILE *stream,
    const en_reconcile_result_t *result,
    en_error_t *error);
```

初期出力先:

- `out/eventnetd/status.jsonl`

将来の出力先:

- local file
- HTTP status API
- GUI backend
- demo用trace log

## 5. runtime generationを整理する場所

現在、Linux VM用runtime script生成は `examples/netns_plan.c` に集中しています。

`eventnetd --once --generate-runtime` では、まず既存binary / scriptを呼ぶ形で十分です。

その後、共通library化するなら以下へ移します。

- `include/eventnet/runtime_plan.h`
- `src/runtime_plan.c`
- `tests/test_runtime_plan.c`

想定する関数:

```c
int en_runtime_plan_generate_netns(
    const en_runtime_plan_input_t *input,
    en_runtime_plan_output_t *output,
    en_error_t *error);
```

生成物:

- `out/netns-runtime/apply-selected.sh`
- `out/netns-runtime/apply-integrated.sh`
- `out/netns-runtime/selected-path.txt`
- `out/netns-runtime/vpp-route-plan.sh`
- `out/netns-runtime/vpp-netns-route-plan.sh`

## 6. adapter実装を置く場所

`eventnetd --once` の後に進める領域です。

### 6.1 strongSwan VICI adapter

実装済みの接合境界:

- `include/eventnet/strongswan_vici_adapter.h`
- `src/strongswan_vici_adapter.c`

現在は `include/eventnet/strongswan_observer.h` / `src/strongswan_observer.c` に、`swanctl --list-sas`出力をchild stateへ変換する依存なしのパーサを実装し、`include/eventnet/strongswan_vici_adapter.h` / `src/strongswan_vici_adapter.c`にVICI callback境界を追加しています。さらに、libviciがある環境では`EVENTNET_ENABLE_STRONGSWAN_VICI`でsocket接続、version request、Tunnel操作、CHILD SA状態取得、有限event購読、切断後の有限回再接続、socket切断までの無期限event購読を行うclient／probeを有効化できます。`eventnet_strongswan_vici_controller_probe`と`eventnetd --vici-monitor-*`では実VICI eventをcontroller reconcileへ接続できます。残る作業は、rekey／DPD等のイベント種別ごとの運用ポリシー、再起動時の購読復旧、service権限の実環境固定です。

役割:

- IKE SA / CHILD SA一覧を取得する。
- CHILD SA up/downをObserved Stateへ変換する。
- DPD / rekey / delete eventをfailure / recovery eventへ変換する。

未踏提出では、VICI購読の本格実装までは必須ではありません。
ただし、設計上の接合部としてheaderとmock実装を置けると説明しやすいです。

### 6.2 VPP adapter

実装済みの接合境界:

- `include/eventnet/vpp_adapter.h`
- `src/vppctl_adapter.c`
- 実装済みの接合境界: `src/vpp_api_adapter.c`

役割:

- route applyの成功 / 失敗をObserved Stateへ反映する。
- interface状態を取得する。
- FIB / route存在確認をする。
- counterをhealth評価へ渡す。

`include/eventnet/vpp_api_adapter.h` / `src/vpp_api_adapter.c`には、VAPI生成コードをcallbackとして注入する薄いadapter境界を実装しています。現時点の既定実装は`vppctl` adapterであり、Binary APIの実transportはVPP SDK依存が揃った環境でcallbackへ接続します。

VPP observerのCLIは`--table TABLE_ID`で対象VRFを明示でき、同一prefixが複数tableにあるFIB出力から指定tableのrouteだけをevent化します。生成eventには検出した`table_id`を任意フィールドとして含め、telemetry parserでも0以上の整数として検証します。

論文前は、`vppctl` command adapterを用いてVPP FIB、VLAN、VRF、interfaceのBackendを実装・検証する。VPP binary APIは後段へ回し、SDKの版依存を論文前の必須条件にはしない。VPP GREの計画生成は実験的境界として保持できるが、実データパスは論文前の必須条件にしない。

将来Backendとして整理する項目:

- VPP GRE interfaceの生成・削除計画。
- GRE内側IPと静的L3 routeの計画生成。
- strongSwanのOS固有IPsec Backendとの差分整理。
- VPP Native IPsec、GRE、SA同期、FIB、疎通を分けたValidateとRollback。

### 6.3 Health probe adapter

Agent側の実測入口として`examples/eventnet_agent.c`を実装済みです。Pathごとのping、RTT、packet loss、jitter、連続成功／失敗回数をJSONL telemetryへ変換し、`eventnetd`の共通health入力へ渡せます。`--yaml`と`--intent`を指定するとYAMLの候補Pathと終端Segmentのremote endpointを自動展開し、複数拠点・Hub・Relay候補を手入力なしで定期測定できます。シミュレーション入力と実ping入力を同じschemaで扱い、CLI値とping出力のstrict validationも行います。Linuxでは`fork`／`exec`、Windowsでは`_popen`を使い、OSごとのping引数（`-c/-W`、`-n/-w`）と`time<1ms`表記を吸収します。segmentがないlegacy routeでは`route_next_hop`を使用します。

本番共通化で追加する場所:

- `include/eventnet/health_probe.h`
- `src/health_probe_ping.c`

残っている役割:

- pingでRTT / lossを測る。
- consecutive failure / recoveryを作る。
- `en_path_health_t` へ変換する。

将来拡張:

- BFD
- FRRouting連携
- VPP counter連携

## 7. daemon loopを置く場所

`examples/eventnetd.c`に、論文評価用の最小daemon loopを実装済みです。`--interval-ms` / `--count`による周期評価、標準入力またはLinux Unix socketからのJSONL受信、`--batch-size`による測定ラウンド化、state file復元、file入力のreload、SIGHUP reload、UID認証、入力レート制限、status JSONL出力までを一つの実行ファイルで確認できます。

本番運用へ拡張する場所:

- `examples/eventnetd.c`から共通reconcile loopを`src/event_loop.c`へ分離する。
- systemd等のservice managerと接続する。
- `--socket-parallel`による有限N接続の同時読み取りは実装済み。無期限接続の同時受信と常駐service化を追加する。
- strongSwan VICIの有限event購読と`monitor-forever`はprobeからeventnetd stdinへ接続可能で、eventnetd自身の`--vici-monitor-*`にも内蔵した。残りはrekey／DPD運用とVPP Binary API観測である。

CLI案:

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --loop \
  --interval-ms 1000 \
  --events out/eventnetd/events.txt \
  --status-json out/eventnetd/status.jsonl
```

残っている処理:

- eventnetd socket reconnect retry / backoff、有限parallel socket受信（実装済み）
- apply planのコマンド実行失敗時は、各apply commandに対応するrollbackを記録し、実行済み操作だけを逆順で戻す処理を実装済み。対応情報がない手作成planは従来のrollback listへ後方互換fallbackする。生成netns runtimeについても失敗時trapと明示rollback scriptを持つ。
- YAMLの`failure_threshold` / `recovery_threshold`、健全active Pathの`hold_down_ms`、品質差分の`hysteresis_percent`による切替抑制を実装済み。
- 無期限parallel接続を含む常駐service化
- 観測状態の永続化と再起動後の自動reconcile

## 8. 論文作成までと未踏期間の順番

論文作成まで:

1. YAML閾値、telemetry schema、adapter API、Explain出力を固定する。
2. Direct／Hub／Relay／VLANのVM評価とFailure／Recovery評価を再現可能にする。
3. `eventnetd`の周期入力、reload、state復元、安全な入力境界を評価する。
4. strongSwan／VPP CLI runtime、rollback、IPsec対象外遮断を評価する。
5. strongSwanのLinux XFRM依存をBackend境界へ閉じ込め、BSD PF_KEY等のOS差分をCapabilityとして整理する。
6. 論文前のVPP GRE計画生成は実データパス未検証として明記し、論文用の合格条件から外す。
7. 実装済み範囲とVICI／VPP Binary APIの未完了codec範囲を文書へ分離する。
8. 論文用に同一telemetryへ異なる閾値を適用する比較手順を固定する。

論文作成後から未踏期間:

1. 物理またはクラウド環境で遅延・Loss・Jitter・帯域を再現する。
2. 閾値、hold-down、hysteresisを変え、切替時間・通信影響・切替頻度を測定する。
3. VPPの対象SDK版を固定し、Binary APIのroute／VRF／VLAN codecを実装する。
4. Graceful Transitionを実通信で評価し、Immediateとの差を定量化する。
5. VPP Native IPsecをstrongSwanとの責任分界、SA同期、観測、rollbackを含む独立Backendとして実装する。
6. VPP GREとVPP Native IPsecを接続し、実データパスを確認する。
7. VPP GRE over IPsecへFRR／BGPの広告・withdrawalを接続する。
8. OSPFを接続し、マルチキャスト、隣接状態、経路収束を評価する。
9. GRE over IPsecとVTIを同じPath Modelで比較し、MTU、収束時間、切替影響、障害観測を測定する。
10. Controller HAを追加し、停止後のActive Path維持と再reconcileを評価する。
11. 余力があればFlow Preserveへ進み、長時間TCPで既存flow維持を測定する。

Node能力のYAML宣言、Path全端点の能力照合、disabled Node除外、未知Node検証は`vm-evaluate.sh node-capability`で再現できます。これは能力profileの宣言・選択境界の評価であり、実際のCloud VPN／FRR backend接続を完了したことを意味しません。

`.github/workflows/ci.yml`では、外部network OSSを必要としないCMake／CTestと全shell scriptの構文をLinux・Windowsで自動検証します。実strongSwan／VPP runtimeは依存とroot権限が必要なため、Linux VMの`paper-evaluation-checklist.md`を別の実証ゲートとして扱います。

## 9. 後回しでよい場所

### 9.1 Hub VPP実データ面の残課題

`VPP_TOPOLOGY=hub`では、site-a／site-bとhub-1のnamespace linkを作成できます。しかし現行の単一VPP instanceへ全portを収容したまま、同一宛先prefixをHub経由とsite-b直結へ同時に転送することはできません。したがって、この設定はHub linkの確認とController生成planの検証用であり、Hub経由の実パケット疎通を完了したものではありません。

実装する場合は、Hub側VPPを独立instanceとして起動するか、VPPのVRF／分離tableと経路リークを含む設計へ変更し、forward／return双方のFIB、VLAN sub-interface、IPsec selectorを同一試験で確認します。

未踏期間の開始時点で、以下は未実装のままでもよいです。

- GUI
- FRRouting本統合
- VPP binary API
- strongSwan VICIのrekey／DPD等を含む運用ポリシーとservice復旧
- Flow Preserve本実装
- systemd unitの実環境適用（テンプレートは追加済み）
- 永続DB
- HA controller

ただし、上記の接合部名を文書とheaderに残しておくと、設計の拡張先を説明しやすくなります。

## 10. 判断基準

次の実装に進むかどうかは、以下で判断します。

- `eventnetd --once` がYAMLとevent fileから同じ選択結果を出せる。
- `status.jsonl` だけを見て、なぜそのPathになったか説明できる。
- `--generate-runtime` で既存のdirect / fallback統合runtime smokeへ接続できる。
- scenario harnessと `eventnetd` が同じreconcile coreを使い始めている。

この状態になれば、未踏提出向けには「設計だけでなく、イベント駆動controllerとして動き始めている」と言えます。
