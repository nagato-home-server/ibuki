# Shell Commands

PathWeaver で使う `scripts/*.sh` の実行方法と引数一覧です。

基本的には repository root で実行します。

```sh
cd controller
```

`sudo` が付いているものは Linux VM 上で root 権限が必要です。

## 1. 最初に使う入口

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/demo-mitou.sh` | `sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/linux-vm-netns.yaml` | `RUN_RUNTIME=1` | 未踏提出向けの一発デモ。build、scenario、runtime plan生成を実行する。`RUN_RUNTIME=1` でrootが必要な統合runtimeも実行する。 |
| `scripts/vm-shell-check.sh` | `sh scripts/vm-shell-check.sh` | なし | なし | `scripts/*.sh` を `sh -n` で構文確認する。 |
| `scripts/vm-check.sh` | `sh scripts/vm-check.sh` | なし | なし | Linux VMに必要な基本コマンドの有無を確認する。 |
| `scripts/vm-runtime-status.sh` | `sh scripts/vm-runtime-status.sh` | なし | なし | strongSwan/VPP/netns/generated files の状態をまとめて確認する。 |

## 2. Build / Plan生成

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-build-cc.sh` | `sh scripts/vm-build-cc.sh` | なし | `CC`, `CFLAGS`, `BUILD_DIR`, `LDLIBS` | CMakeなしで`cc`によりbuildする。既定で`-O2`、stack protector、FORTIFYを有効にする。成果物は既定で`build-linux-cc/`。VPP route observerに加えてVLAN sub-interface observerも生成する。 |
| `scripts/vm-build.sh` | `sh scripts/vm-build.sh` | なし | `BUILD_DIR`, `VPP_PREFIX`, `EVENTNET_ENABLE_VPP_API`, `EVENTNET_ENABLE_STRONGSWAN_VICI`, `EVENTNET_ENABLE_HARDENING` | CMake buildを実行する。Linuxでは`EVENTNET_ENABLE_HARDENING=ON`が既定で、stack protector、FORTIFY、RELROを有効にする。成果物は既定で`build-linux/`。依存が揃う場合は環境変数を`ON`にして実transport有効ビルドを行い、標準外VPP SDKは`VPP_PREFIX`でconfigureへ渡す。既存buildが別作業フォルダを指す場合はstale cacheとして終了する。 |
| CMake install | `cmake --install build --prefix /usr/local` | なし | `CMAKE_INSTALL_PREFIX` | `eventnetd`を`libexec/ibuki`、その他のCLIを`bin`、systemd unitを`lib/systemd/system`へ再現可能に配置する。 |
| `scripts/vm-generate-plan.sh` | `sh scripts/vm-generate-plan.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/ipsec-routes.yaml` | `OUT_DIR` | YAMLから `swanctl.conf` と apply script を生成する。 |
| `scripts/vm-generate-netns-runtime.sh` | `sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml --active-path path-direct --fail-path path-direct` | `[yaml] [eventnet_netns_plan args...]` | `OUT_DIR` | controller選択結果から netns runtime script、rollback script、VPP route planを生成する。segmentなしlegacy routeはYAMLの宛先prefix／next-hopによる一方向planを生成する。2個目以降の引数は `eventnet_netns_plan` に渡す。 |

## 3. Scenario / Controller Smoke

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-eventnet-scenario-smoke.sh` | `sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/linux-vm-netns.yaml` | `OUT_DIR` | direct、fallback、evaluated、multi-step scenario をまとめて検証する。 |
| `scripts/vm-agent-smoke.sh` | `sh scripts/vm-agent-smoke.sh [YAML]` | `[YAML]` | `BUILD_DIR`, `OUT_DIR` | Agentのhealthy/failed telemetry、指定YAMLから展開したDirect/Hub/Relayの3 Path、Relay終端endpoint、eventnetdへのbatch反映、`ping`が利用可能なLinuxでのloopback実測値、不正引数拒否を確認する。 |
| `scripts/vm-agent-eventnetd-live-smoke.sh` | `LIVE_COUNT=4 LIVE_INTERVAL_MS=250 sh scripts/vm-agent-eventnetd-live-smoke.sh samples/linux-vm-netns.yaml` | `[YAML]` | `BUILD_DIR`, `OUT_DIR`, `INTENT_ID`, `LIVE_COUNT`, `LIVE_INTERVAL_MS` | Agentが1件ずつtelemetryを追記し、eventnetdが同じファイルを周期再読込して指定回数reconcileすることを確認する。 |
| `scripts/vm-threshold-smoke.sh` | `sh scripts/vm-threshold-smoke.sh samples/route-examples.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR` | Agentの連続成功／失敗回数をeventnetdとstate fileへ渡し、単発障害維持、閾値fallback、復旧閾値到達を確認する。実VPP／strongSwanは使用せず、mock backendでPath選択の再現性を検証する。 |
| `scripts/vm-xfrm-policy-smoke.sh` | `sh scripts/vm-xfrm-policy-smoke.sh samples/route-examples.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR` | `block_non_ipsec: true`のIntentでTunnel selector限定の双方向XFRM block／rollbackを生成し、既定Intentでは生成しないことを確認する。実kernelへの適用はroot権限のLinux VMで別途行う。 |
| `scripts/vm-xfrm-policy-runtime-smoke.sh` | `sudo sh scripts/vm-xfrm-policy-runtime-smoke.sh` | なし | `SOURCE_PREFIX`, `DESTINATION_PREFIX`, `BLOCK_PRIORITY` | site-a/site-b namespaceへselector限定のXFRM blockを実適用し、policy一覧とpriorityを確認する。終了時に追加policyを削除する。 |
| `scripts/vm-xfrm-cleartext-smoke.sh` | `sudo sh scripts/vm-xfrm-cleartext-smoke.sh` | なし | `SOURCE_PREFIX`, `DESTINATION_PREFIX`, `BLOCK_PRIORITY`, `RUN_BASE` | direct IPsecを起動して暗号化疎通を確認し、block policy追加後も疎通できること、SA停止後のcleartext疎通が拒否されることを確認する。終了時にSA／XFRMをcleanupする。 |
| `scripts/vm-runtime-rollback-smoke.sh` | `sudo sh scripts/vm-runtime-rollback-smoke.sh` | なし | `BUILD_DIR`, `OUT_DIR` | 生成direct runtimeへテスト失敗を注入し、自動rollbackでcharon／XFRM stateが撤去されることを確認する。 |
| `scripts/vm-stability-smoke.sh` | `sh scripts/vm-stability-smoke.sh` | なし | `BUILD_DIR`, `OUT_DIR` | YAMLのevaluated Intentへ3ラウンドのtelemetryを入力し、hysteresisによるactive維持と十分な品質改善後の切替を確認する。 |
| `scripts/vm-evaluate.sh telemetry` | `sh scripts/vm-evaluate.sh telemetry samples/linux-vm-netns.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR` | Agent telemetryとeventnetdのbatch、freshness、stdin、Unix socket、status JSONLの統合を評価レポートへ記録する。 |
| `scripts/vm-evaluate.sh telemetry-long` | `LONG_COUNT=10 LONG_INTERVAL_MS=1000 sh scripts/vm-evaluate.sh telemetry-long samples/linux-vm-netns.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR`, `LONG_COUNT`, `LONG_INTERVAL_MS`, `INTENT_ID` | YAML／Intent候補をAgentが周期測定し、生成Path数をbatch sizeとしてeventnetdへ渡す。指定回数のreconcile、status JSONL、state file更新を確認する。 |
| `scripts/vm-evaluate.sh telemetry-live` | `LIVE_COUNT=10 LIVE_INTERVAL_MS=250 sh scripts/vm-evaluate.sh telemetry-live samples/linux-vm-netns.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR`, `LIVE_COUNT`, `LIVE_INTERVAL_MS`, `INTENT_ID` | Agentの逐次追記とeventnetdの周期再読込を実時間で確認する。 |
| `scripts/vm-evaluate.sh threshold` | `sh scripts/vm-evaluate.sh threshold samples/linux-vm-netns.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR`, `THRESHOLD_YAML` | Agent連続成功／失敗回数によるPath維持、fallback、recoveryを評価する。第2引数は通常表示用YAMLで、対象は`THRESHOLD_YAML`（既定: `samples/route-examples.yaml`）である。 |
| `scripts/vm-evaluate.sh xfrm-policy` | `sh scripts/vm-evaluate.sh xfrm-policy samples/linux-vm-netns.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR`, `XFRM_YAML` | Tunnel selector限定の双方向XFRM block／rollback生成と、既定Intentでのblock無効を評価する。 |
| `eventnet_agent` 数値入力 | `build-linux-cc/eventnet_agent --path path-direct --target 203.0.113.9 --simulate 12.5 0 --count 3` | `N`, `RTT`, `LOSS` | `count>=1`, `interval-ms>=0`, `RTT>=0`, `0<=LOSS<=100` | 数値はstrict parseされ、文字列・範囲外・NaN・Infは終了コード2で拒否する。 |
| `eventnet_scenario --health` | `build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml --health path-direct=healthy,rtt=20,loss=0` | `PATH_ID`, `RTT`, `LOSS` | Path IDは許可文字、RTT>=0、0<=LOSS<=100 | 未知属性、数値不正、Path IDの過長・不正文字は終了コード2で拒否する。 |
| `eventnet_agent --probe` | `build-linux-cc/eventnet_agent --source site-a --probe path-direct 203.0.113.9 --probe path-via-hub 203.0.113.13 --count 10 --output out/telemetry.jsonl` | `PATH_ID`, `IP` | なし | 1ラウンドで複数Pathを測定し、RTT、packet loss、PathごとのRTT差分jitterを含むJSONLを生成する。 |
| `eventnet_agent --yaml` | `build-linux-cc/eventnet_agent --yaml samples/linux-vm-netns.yaml --intent intent-a-b --count 10 --interval-ms 1000 --output out/telemetry.jsonl` | `YAML`, `INTENT_ID` | `--simulate` | YAMLのexplicit／priority／evaluated Intentに対応するPathと終端Segmentのremote endpointを自動展開し、候補Pathをまとめて定期測定する。segmentがないlegacy routeは`route_next_hop`を使用する。 |
| `eventnet_agent --append` | `build-linux-cc/eventnet_agent --path path-direct --target 203.0.113.9 --count 1 --append --output out/telemetry.jsonl` | `PATH_ID`, `IP` | `--append`には`--output`必須 | 既存JSONLを保持したままAgent telemetryを追記する。live Agent／eventnetd連携で使用する。 |
| `scripts/vm-telemetry-controller-smoke.sh` | `sh scripts/vm-telemetry-controller-smoke.sh` | なし | `BUILD_DIR`, `OUT_DIR` | Agent JSONLを`eventnetd`へ渡し、ファイル・標準入力・Unixソケット、source省略時の互換性、実測値によるDirect/Hub選択、複数Pathの3ラウンドDirect→Hub→Direct切替、status JSONL、古いtelemetryの除外を確認する。 |
| `eventnetd --telemetry-stdin` | `build-linux-cc/eventnet_agent ... | build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry-stdin --count 10` | `YAML` | `--max-age-ms` | AgentのJSONLを標準入力から逐次受信し、同一Controller状態で再評価する。 |
| `eventnetd --telemetry FILE` replay | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry out/telemetry.jsonl --batch-size 3 --count 10` | `YAML`, `FILE`, `N` | 最大`count` batch、各N records | JSONL fileをstreamとして読み、N件ごとに1回reconcileする。Agentの複数Path×周期出力を後から再現する用途に使う。Linuxではsymlinkと非regular fileを拒否する。 |
| `samples/telemetry-replay.jsonl` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry samples/telemetry-replay.jsonl --batch-size 3 --count 1 --max-age-ms 0` | `FILE` | 3 records | file replayの最小fixture。direct／hub／relayを1 batchとして読み、controllerがdirectを選択することを確認する。固定timestampを使うためfreshness無効化を明示する。 |
| `samples/telemetry-replay-fallback.jsonl` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry samples/telemetry-replay-fallback.jsonl --batch-size 3 --count 2 --max-age-ms 0` | `FILE` | 6 records | direct healthyの1 batch後にdirect failedを入力し、2 batch目でHub fallbackへ切り替わることを確認する。 |
| `eventnet_file_replay_incomplete` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry samples/telemetry-replay.jsonl --batch-size 3 --count 2 --max-age-ms 0` | `FILE` | 不足入力 | 要求した2 batchのうち1 batchしか到着しない場合、成功扱いせず終了コード1になることを確認する。 |
| `eventnetd --batch-size` | `eventnet_agent --probe ... | eventnetd samples/linux-vm-netns.yaml --telemetry-stdin --batch-size 3 --count 1` | `N` | `1..EN_MAX_PATHS` | 複数Pathの1測定ラウンドをまとめてhealthへ反映してから選択する。途中でEOFになった未完了batchは評価しない。常駐streamでは一時的な`no_candidate`を次batchまで待つ。 |
| `eventnetd --telemetry-socket` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry-socket /run/ibuki/eventnetd.sock --socket-accept-count 0 --socket-retry-count 3 --socket-retry-backoff-ms 500 --state-file out/eventnetd/state.tsv` | `YAML`, `PATH`, `N` | `N>=0`; `N=0`または`N>1`は`--state-file`必須 | LinuxのUnixドメインソケットからAgent JSONLを受信する。既定は1接続、0は無期限。`N>1`では共有batchへ集約し、`--socket-parallel`を付けるとN接続をpollで同時読み取りして同じController状態で再評価する。`--socket-parallel-timeout-ms`で受信待機上限を設定する。`N=0`は従来の逐次常駐処理を行う。認証拡張は未実装。 |
| `vm-eventnetd-status-security-smoke.sh` | `sh scripts/vm-eventnetd-status-security-smoke.sh samples/linux-vm-netns.yaml` | `[yaml]` | Linux、`ln`、`chmod` | eventnetdのstatus JSONL出力について、通常ファイルへの追記成功、symlink拒否、group書き込み可能ファイル拒否を確認する。 |
| `vm-evaluate.sh status-output-security` | `sh scripts/vm-evaluate.sh status-output-security samples/linux-vm-netns.yaml` | `[yaml]` | Linux、eventnetd、`ln`、`chmod` | status出力の安全性を評価レポートへ記録する。 |
| `eventnetd` 停止 | `kill -TERM <eventnetd-pid>` | Linux, socket mode | なし | socket待機中のeventnetdを安全に停止し、socket pathを後始末する。 |
| `eventnetd --status-jsonl` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --status-jsonl out/status.jsonl --count 10` | `YAML`, `FILE` | なし | 各reconcile結果を`ibuki.status.v1` JSONLとして追記する。 |
| `eventnetd --state-file` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --state-file out/eventnetd.state --once` | `YAML`, `FILE` | なし | 前回の`traffic_key`とPathを起動時に復元し、現在のIntentのtraffic keyと一致するstateだけを受け入れ、成功判断後にTSVを更新する。 |
| `eventnetd --reload-config` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --reload-config --state-file out/eventnetd.state --interval-ms 1000 --count 10` | `YAML`, `FILE`, `FILE` | `--state-file`必須 | 周期ごとにYAMLを再読込し、前回の適用状態を復元して再評価する。file入力専用で、reload失敗時はその周期を適用せず終了する。 |
| `eventnet_yaml_demo`（pubkey） | `build-linux-cc/eventnet_yaml_demo samples/cert-auth.yaml --check-cert-files --check-cert-validity 86400` | `YAML`, `SECONDS` | `auth_method: pubkey`時は`local_cert`必須; validityはLinuxのみ | strongSwan証明書認証を含むTunnel定義を読み、証明書・CAファイルのreadable状態と、指定秒数以上の有効期間をOpenSSLで確認する。秘密鍵はYAMLへ置かない。 |
| `eventnet_yaml_demo --validate-only` | `build-linux-cc/eventnet_yaml_demo --validate-only samples/route-examples.yaml` | `YAML` | なし | 全Intentの実行や品質測定を行わず、YAMLの構文・参照整合性だけを検証する。複数のroute記法をCIや編集時に一括確認する用途。 |
| `vm-evaluate.sh cert-auth` | `EVAL_REPEAT=1 sh scripts/vm-evaluate.sh cert-auth samples/cert-auth.yaml` | `YAML` | Linux・OpenSSL必須 | 一時X.509証明書を生成し、pubkey設定のreadabilityと24時間以上の有効期間を再現可能に検証する。 |
| `eventnetd --max-records-per-second` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry-stdin --max-records-per-second 20 --count 10` | `N` | `N>=0`; `0`は無制限 | Agent入力を1秒あたりNレコード以下に制限し、異常な高頻度入力による再評価・backend操作の集中を抑える。 |
| `eventnetd` 数値入力 | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --count 10 --interval-ms 1000` | `count`, `interval-ms`, `batch-size`, `max-age-ms`, `socket-accept-count`, `socket-retry-count`, `socket-retry-backoff-ms`, `socket-uid` | 各値は非負（UIDは`-1`または非負） | daemonの運用パラメータはstrict parseされ、文字列・符号付き不正値・範囲外は終了コード2で拒否する。 |
| `eventnetd --socket-uid` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry-socket /run/ibuki/eventnetd.sock --socket-uid "$(id -u ibuki-agent)"` | `UID` | `UID>=0`; Linuxのみ | Unix socketの接続元peer UIDを`SO_PEERCRED`で検証する。認証を使う場合はsocketの所有者・権限設定と併用する。 |
| `eventnetd systemd` | `sudo install -m 0644 deploy/ibuki-eventnetd.service /etc/systemd/system/ibuki-eventnetd.service && sudo systemctl daemon-reload && sudo systemctl enable --now ibuki-eventnetd.service` | なし | Linux, systemd | `docs/eventnetd-service.md`のunitを配置し、常駐socket、state保存、異常時再起動を確認する。環境固有のUID・socket権限・VICI／VPP接続先を設定してから起動する。 |
| `vm-evaluate.sh service-unit` | `sh scripts/vm-evaluate.sh service-unit samples/linux-vm-netns.yaml` | なし | `OUT_DIR`, `systemd-analyze`（任意） | systemd unitのExecStartPre、再起動、権限制限、PrivateDevices、kernel保護、address family制限、socket無期限受信、`--apply`なしのdry-run既定を静的検査する。 |
| `eventnetd --backend command` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --once` | `YAML` | `--apply` | strongSwan/VPP command adapterを使う。標準はdry-run、`--apply`で実行する。 |
| `eventnetd --swanctl-uri` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --swanctl-uri unix:///run/eventnet-netns-ipsec-direct/site-a/charon.vici --once` | `URI` | `--apply` | namespace別VICI socketを`swanctl --uri`へ渡す。URIは安全な文字種に制限する。 |
| `eventnetd --swanctl-config` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --swanctl-uri unix:///run/eventnet/site-a/charon.vici --swanctl-config out/eventnet-swanctl.conf --once` | `FILE` | `--apply` | eventnetdが指定VICI socketへconnection設定を一度loadしてからTunnelを開始する。既存の外部`--load-conns`手順も利用できる。 |
| `eventnetd --vppctl-socket` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --vppctl-socket /run/vpp/cli.sock --once` | `PATH` | `--apply` | `vppctl -s PATH`へroute操作を向ける。Path計画とVPP CLI接続先を分離する。 |
| `eventnetd --verify-swanctl` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --swanctl-uri unix:///run/eventnet/site-a/charon.vici --verify-swanctl --once` | なし | `--apply` | Tunnel開始後に`--list-sas --child`を実行し、SA確認失敗をTransition失敗へ渡す。 |
| `eventnetd --verify-vpp` | `build-linux-cc/eventnetd samples/linux-vm-netns.yaml --telemetry out/telemetry.jsonl --backend command --vppctl-socket /run/vpp/cli.sock --verify-vpp --apply --once` | なし | `--apply`, `--backend command` | route apply後に`show ip fib`を取得し、期待prefixの存在をVPP observerで確認する。 |
| `scripts/vm-plan-secret-permission-smoke.sh` | `sh scripts/vm-plan-secret-permission-smoke.sh` | なし | `BUILD_DIR`, `OUT_DIR` | 生成swanctl.confが0600で作成され、既存symlink経由の上書きが拒否されることをLinuxで確認する。 |
| `scripts/vm-vpp-api-preflight.sh` | `sh scripts/vm-vpp-api-preflight.sh` | なし | `VPP_PREFIX` | VPP Binary APIの`vapi/vapi.h`と`libvapi.so`／`libvapiclient.so`／`libvppapiclient.so`を検出する。標準外prefixは`VPP_PREFIX=/opt/vpp`で指定でき、見つからなくてもCLI adapterは利用可能。 |
| `scripts/vm-vpp-api-discover.sh` | `sh scripts/vm-vpp-api-discover.sh` | なし | `VPP_PREFIX`, `OUT_FILE` | Linux VMのVPP version、生成header、API JSON、route／VRF／VLAN関連symbolを収集し、SDK版依存のmessage codec実装用資料を`out/vpp-api-discovery.txt`へ保存する。 |
| `vm-evaluate.sh vpp-api` | `sh scripts/vm-evaluate.sh vpp-api samples/linux-vm-netns.yaml` / `RUN_VPP_API=1 sh scripts/vm-evaluate.sh vpp-api samples/linux-vm-netns.yaml` | `[yaml]` | `OUT_DIR`, `RUN_VPP_API`, `RUN_VPP_API_DISCOVER`, `VPP_API_PROBE` | VPP Binary API依存の有無を`skip`／`partial`で記録し、既定でSDK version・生成header・API JSONのdiscoveryも`out/evaluation/*/vpp-api-discovery.txt`へ保存する。`RUN_VPP_API=1`ではSDK有効ビルドの接続probeも実行し、route/VLAN message未実装はpartialとして残す。 |
| `eventnet_vpp_observer --table` | `build-linux-cc/eventnet_vpp_observer out/show-ip-fib.txt 10.10.2.0/24 --event path-direct route-a-b --table 100` | `TABLE_ID>=0` | VPP FIBのVRF header | 指定VRFのFIB routeだけをobserverで選び、`table_id`・prefix・next-hop付きの`ibuki.event.vpp.route.v1`へ変換する。eventnetdはPath routeと一致しないroute identityを拒否する。 |
| `en_vpp_api_adapter` | C/CMakeで組み込み | VAPI生成callback | なし | VPP SDK依存の操作callbackを`en_vpp_adapter_t`へ、route観測callbackを`en_vpp_route_observation_t`へ接続する薄い境界。Binary APIのmessage処理はtransport実装側で提供する。callback未設定は`EN_ERR_INVALID_ARGUMENT`。 |
| `docs/vpp-api-implementation.md` | 作業者向け手順 | なし | `VPP_PREFIX`, `EVENTNET_ENABLE_VPP_API` | VPP SDK確認、Binary API transport実装順、受け入れ条件を確認する。 |
| `en_strongswan_vici_adapter` | C/CMakeで組み込み | VICI callback | なし | libvici依存のTunnel操作を`en_strongswan_adapter_t`へ、CHILD SA観測callbackを`en_strongswan_sa_observation_t`へ接続する薄い境界。実VICI message処理はtransport実装側で提供する。callback未設定は`EN_ERR_INVALID_ARGUMENT`。 |
| `eventnet_strongswan_vici_probe` | `cmake -S . -B build-vici -DEVENTNET_ENABLE_STRONGSWAN_VICI=ON && cmake --build build-vici --config Debug` / `... version` / `... initiate tun-a-b` / `... terminate tun-a-b` / `... observe tun-a-b` / `... monitor tun-a-b path-direct 30000 3 500` | `URI`, `version|initiate|terminate|observe|monitor`, `CHILD_ID`, `PATH_ID`, `DURATION_MS`, `RETRY_COUNT`, `BACKOFF_MS` | libvici、VICI socket権限 | 実libviciで接続し、version確認、明示CHILDの開始・終了、`list-sas`状態取得、または有限時間の`child-updown`購読を行う。monitorはsocket切断・接続失敗時に指定回数だけ再接続し、`tunnel.v1` JSONLを標準出力へ出す。 |
| `en_strongswan_parse_list_sas` | Cテストで実行 | `swanctl --list-sas`出力 | なし | `swanctl --list-sas`のchild stateをObserved Tunnelのstate/healthへ変換する。VICI接続前の解析境界。 |
| `eventnet_swanctl_observer` | `build-linux-cc/eventnet_swanctl_observer samples/swanctl-list-sas-installed.txt tun-a-b` / `swanctl --list-sas --child tun-a-b \| build-linux-cc/eventnet_swanctl_observer - tun-a-b --event path-direct` | `LIST_SAS_OUTPUT`, `CHILD_ID`, `PATH_ID` | なし | 保存または標準入力の`swanctl --list-sas`出力をObserved state/healthまたは`ibuki.event.tunnel.v1` JSONLへ変換する。 |
| `eventnet_vpp_observer` | `build-linux-cc/eventnet_vpp_observer samples/vpp-show-ip-fib.txt 10.10.2.0/24` / `vppctl show ip fib \| build-linux-cc/eventnet_vpp_observer - 10.10.2.0/24 --event path-direct route-a-b` | `SHOW_IP_FIB_OUTPUT`, `DESTINATION_PREFIX`, `PATH_ID`, `ROUTE_ID` | なし | 保存または標準入力の`show ip fib`出力をroute観測または`ibuki.event.vpp.route.v1` JSONLへ変換する。VRFを選ぶ場合は`--table TABLE_ID`を追加する。 |
| `eventnet_vpp_interface_observer` | `build-linux-cc/eventnet_vpp_interface_observer samples/vpp-show-interface.txt host-vpp-site-a.100` / `vppctl show interface \| build-linux-cc/eventnet_vpp_interface_observer - host-vpp-site-a.100 --event path-vpp-vlan` | `SHOW_INTERFACE_OUTPUT`, `INTERFACE`, `PATH_ID` | なし | `show interface`出力をinterface観測または`ibuki.event.vpp.interface.v1` JSONLへ変換する。eventnetdはVLAN付きIntentの指定sub-interfaceとidentityを照合する。 |
| `vm-observer-eventnetd-runtime-smoke.sh` | `sudo INTENT_ID=intent-a-b VPP_TABLE_ID=100 sh scripts/vm-observer-eventnetd-runtime-smoke.sh samples/linux-vm-netns.yaml` | `[YAML]` | `RUN_BASE`, `VPP_SOCKET`, `VPP_TABLE_ID`, `INTENT_ID`, `PATH_ID`, `CHILD_ID`, `ROUTE_PREFIX`, `ROUTE_ID`, `OUT_DIR` | 実strongSwan `swanctl --list-sas`と実VPP `show ip fib`をobserver eventへ変換し、同じeventnetdへ入力してPath identityと2件の受理を確認する。`VPP_TABLE_ID`指定時は対象VRFを選ぶ。 |
| `vm-evaluate.sh observer-runtime` | `sudo INTENT_ID=intent-a-b sh scripts/vm-evaluate.sh observer-runtime samples/linux-vm-netns.yaml` | `[YAML]` | `RUN_BASE`, `VPP_SOCKET`, `INTENT_ID`, `OUT_DIR` | IPsec/VPP runtimeを起動済みの状態で、実strongSwan/VPP observer eventを同じeventnetdへ入力する評価ケース。`all`からは自動実行せず、runtime起動後に明示実行する。 |
| `vm-vpp-vlan-netns-smoke.sh` | `sudo sh scripts/vm-vpp-vlan-netns-smoke.sh` | なし | `YAML`, `OUT_DIR`, `VLAN_ID` | Controller生成のVLAN sub-interface/route planをVPPへ適用し、タグ付きLANの疎通と、`deny_unmatched_vlan`による親interface経由の未タグ通信遮断を確認する。標準YAMLのVLAN 100を指定値へ置換する。 |
| `eventnetd --telemetry-socket` + `socat` | `socat - UNIX-CONNECT:/run/ibuki/eventnetd.sock < out/telemetry.jsonl` | `SOCKET` | なし | LinuxでAgent JSONLをローカルUnixソケットへ送る。複数Agentを順次接続する場合は`--socket-accept-count`と`--state-file`を指定する。 |
| `vm-eventnet-event-smoke.sh` | `sh scripts/vm-eventnet-event-smoke.sh` | なし | `BUILD_DIR`, `OUT_DIR` | `path_failed`でHubへfallbackし、`path_recovered`でDirectへ戻るイベント入力をファイルとUnix socketの連続streamで再現する。strongSwan Tunnel identity不一致拒否、command backendのVICI config load、explicit routeのVRF table準備、VLAN sub-interface作成、VPP socket経由投入、VLAN interface eventのup/downとidentity照合、双方向XFRM block、既存XFRM policy再利用、有限parallel socket、timeout、SIGTERM停止時のstate保持、malformed input拒否も確認する。 |
| `vm-evaluate.sh event-reconcile` | `sh scripts/vm-evaluate.sh event-reconcile samples/linux-vm-netns.yaml` | `YAML` | `BUILD_DIR`, `OUT_DIR` | イベント入力のfallback/recovery、Unix socket連続処理、有限multi-Agent shared-batch、parallel timeout終了を評価レポートへ記録する。 |
| `scripts/vm-route-yaml-smoke.sh` | `sh scripts/vm-route-yaml-smoke.sh samples/route-examples.yaml` | `[yaml]` 省略時 `samples/route-examples.yaml` | `OUT_DIR`, `BUILD_DIR` | YAMLのroute記法をまとめて検証する。旧式単一路由、segmentなしlegacy route、明示route、双方向、node別、table/metric/interface付きrouteを確認する。route属性ケースでは、YAMLの`metric`がVPP CLIの`preference`へ変換され、interface・tableと同時に出力されること、VPP CLI socket分岐が生成されることを確認する。空のsegmentなしPathと、実行先が曖昧なexplicit routes付きsegmentなしPathの拒否も確認する。 |
| `scripts/vm-netns-controller-smoke.sh` | `sh scripts/vm-netns-controller-smoke.sh recovery samples/linux-vm-netns.yaml` | `[switch\|fallback\|recovery] [yaml]` | `MODE` | controller-generated netns runtimeを使い、direct/hub/recoveryの切替を検証する。rootではなく通常ユーザーで実行する。 |
| `scripts/vm-controller-integrated-runtime-smoke.sh` | `sudo MODE=fallback sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/linux-vm-netns.yaml` | `MODE=direct\|fallback\|both` | controller-generated planから IPsec と VPP forwarding を連続制御する統合smoke。 |
| `scripts/vm-evaluate.sh` | `sh scripts/vm-evaluate.sh all samples/linux-vm-netns.yaml` | `[case] [yaml]` | `OUT_DIR`, `EVAL_REPEAT`, `RUN_RUNTIME=1` | 今後の評価で確認すべき点をまとめて実行し、`out/evaluation/` にMarkdown/CSVレポートと実行環境manifest（時刻、OS/kernel、git revision、dirty状態、作業ツリー差分SHA-256、使用バイナリ、YAML／主要binaryのSHA-256）を出す。VPP observer caseではVRF table選択からeventnetd受理まで確認する。未実装項目は`skip` / `partial`として明示する。`telemetry-long`ではAgentとeventnetd双方の周期を検証し、`all`は個別caseが失敗しても残りのcaseを継続して結果を収集する。 |
| `vm-evaluate.sh vici-runtime` | `VICI_URI=unix:///run/strongswan/charon.vici VICI_CHILD=tun-a-b VICI_PATH=path-direct VICI_BUILD_DIR=build-vici sh scripts/vm-evaluate.sh vici-runtime samples/linux-vm-netns.yaml` | `VICI_URI`, `VICI_CHILD`, `VICI_PATH`, `VICI_DURATION_MS`, `VICI_RETRY_COUNT`, `VICI_BACKOFF_MS`, `VICI_BUILD_DIR` | libvici probe、VICI socket、対象CHILD | version、`list-sas` observe、有限`child-updown` monitorと再接続を評価する。duration／retry／backoffは厳格な数値検証を行う。probe未構築または必須環境変数未指定時はskipする。 |
| `vm-evaluate.sh vici-controller` | `VICI_URI=unix:///run/strongswan/charon.vici VICI_BUILD_DIR=build-vici sh scripts/vm-evaluate.sh vici-controller samples/cert-auth.yaml` | `VICI_URI`, `VICI_CONTROLLER_PROBE`, `VICI_CONTROLLER_YAML`, `VICI_CONTROLLER_INTENT`, `VICI_BUILD_DIR` | libvici controller probe、VICI socket | 実VICI clientのTunnel操作・CHILD再観測をcontroller reconcileまで検証する。probe未構築またはsocket未指定時はskipする。 |
| `vm-evaluate.sh vici-eventnetd-internal` | `sudo VICI_URI=unix:///run/strongswan/charon.vici VICI_CHILD=tun-a-b VICI_PATH=path-direct VICI_BUILD_DIR=build-vici sh scripts/vm-evaluate.sh vici-eventnetd-internal samples/linux-vm-netns.yaml` | `VICI_URI`, `VICI_CHILD`, `VICI_PATH`, `VICI_DURATION_MS`, `VICI_BUILD_DIR` | libvici有効eventnetd、VICI socket、VPP CLI | eventnetd自身のVICI購読、イベント再reconcile、SIGTERM／SIGINT停止を評価する。libvici有効ビルドまたは実socketがない場合はskip／failとして記録する。 |
| `vm-vici-eventnetd-smoke.sh` | `sudo VICI_URI=unix:///run/strongswan/charon.vici VICI_BUILD_DIR=build-vici sh scripts/vm-vici-eventnetd-smoke.sh` / `sudo RUN_INTERNAL=1 VICI_URI=unix:///run/strongswan/charon.vici VICI_BUILD_DIR=build-vici sh scripts/vm-vici-eventnetd-smoke.sh` | `VICI_URI`, `VICI_CHILD`, `VICI_PATH`, `VICI_DURATION_MS`, `VICI_RETRY_COUNT`, `VICI_BACKOFF_MS`, `VICI_BUILD_DIR`, `BUILD_DIR`, `YAML`, `OUT_DIR`, `RUN_CONTROLLER_PROBE`, `RUN_INTERNAL` | libvici probe、strongSwan VICI socket、eventnetd | 通常はVICI monitorのJSONLをeventnetd stdinへ接続し、`RUN_INTERNAL=1`ではeventnetd自身の購読で同じ再確立を確認する。既存SAを一度terminateしてからinitiateするため、検証用環境で実行する。 |
| `eventnet_strongswan_vici_probe monitor-forever` | `build-vici/eventnet_strongswan_vici_probe unix:///run/strongswan/charon.vici monitor-forever tun-a-b path-direct 0 3 500 | build-linux-cc/eventnetd samples/linux-vm-netns.yaml --intent intent-a-b --telemetry-stdin --count 0 --state-file /var/lib/ibuki/state.tsv` | `VICI_URI`, `VICI_CHILD`, `VICI_PATH`, `VICI_RETRY_COUNT`, `VICI_BACKOFF_MS` | Linux、libvici、VICI socket | VICI socketを切断まで保持し、`child-updown` event JSONLをeventnetdへ継続配信する。`0`は無期限を意味し、停止時は外側のservice managerからSIGTERMを送る。 |
| `eventnet_strongswan_vici_controller_probe` | `build-vici/eventnet_strongswan_vici_controller_probe unix:///run/strongswan/charon.vici samples/cert-auth.yaml intent-cert-a-b` | `VICI_URI`, YAML, optional `INTENT_ID` | Linux、libvici、VICI socket | YAMLからcontrollerを構成し、実VICIのTunnel操作・CHILD再観測をcontroller reconcileへ接続する。VPPはmockであり、VICI接合単体の確認に使う。 |
| `eventnet_strongswan_vici_controller_probe --monitor` | `build-vici/eventnet_strongswan_vici_controller_probe unix:///run/strongswan/charon.vici samples/cert-auth.yaml intent-cert-a-b --monitor tun-cert path-cert 0` | `VICI_URI`, YAML, CHILD、Path、`DURATION_MS` | Linux、libvici、VICI socket | VICIの`child-updown`イベントをtelemetryとして解釈し、同じcontrollerへ再reconcileする。`DURATION_MS=0`は停止まで監視する。VPPはmock。 |
| `eventnetd --vici-monitor-*` | `build-vici/eventnetd samples/cert-auth.yaml --backend command --swanctl-uri unix:///run/strongswan/charon.vici --vici-monitor-child tun-cert --vici-monitor-path path-cert --vici-monitor-duration-ms 60000 --state-file out/eventnetd.state` | URI、CHILD、Path、`DURATION_MS` | Linux、libvici、VICI socket、VPP CLI | eventnetd自身がVICIの`child-updown`を購読し、共通telemetry identity検証、再reconcile、status JSONL、state保存を行う。`DURATION_MS=0`はSIGTERM／SIGINTで停止する。`EVENTNET_ENABLE_STRONGSWAN_VICI=ON`のビルドが必要。 |
| `eventnet_vpp_api_transport_probe` | `build-linux/eventnet_vpp_api_transport_probe [APP_NAME] [CHROOT_PREFIX]` | `EVENTNET_ENABLE_VPP_API=ON`でビルド | Linux、VPP SDK、VPP API endpoint | route変更なしでVPP Binary APIへ接続し、受信FDを取得して切断する。libraryには生成message用のallocate／send／free境界を持つが、route／VLAN／VRFの具体的codecは未実装。 |
| `vm-evaluate.sh vlan-policy` | `sh scripts/vm-evaluate.sh vlan-policy samples/linux-vm-netns.yaml` | なし | `VLAN_YAML`, `VLAN_MATRIX` | VLAN ID 1/100/4094（`VLAN_MATRIX`で変更可能）のVPP-only runtime、summary、sub-interface、VLAN route、deny ACL生成、一覧外VLANのplan拒否に加え、Hub waypointの複数VPP port・未タグACL・table route planを検証する。既定の検証YAMLは`VLAN_YAML`または`samples/vpp-vlan-netns.yaml`。 |
| `vm-evaluate.sh node-capability` | `sh scripts/vm-evaluate.sh node-capability samples/node-capabilities.yaml` | `[yaml]` | `NODE_CAPABILITY_YAML`, `BUILD_DIR`, `OUT_DIR`, `INTENT_ID` | Nodeのrole/capability表示、Intentの`required_capabilities`によるPath選択、未知Node・disabled Nodeを含むモデル境界を評価する。 |
| `vm-evaluate.sh route-yaml` | `sh scripts/vm-evaluate.sh route-yaml samples/route-examples.yaml` | なし | `ROUTE_YAML` | 対応するroute YAML形式を一括検証し、不正routeの拒否も確認する。 |
| `vm-eventnet-sighup-smoke.sh` | `sh scripts/vm-eventnet-sighup-smoke.sh` | Linux | `BUILD_DIR` | eventnetdへSIGHUPを送り、正常reloadと不正設定時の旧設定維持を確認する。 |
| `eventnetd --reload-on-sighup` | `eventnetd config.yaml --telemetry telemetry.jsonl --reload-config --reload-on-sighup --state-file out/state.tsv --count 1` | Linux, SIGHUP | `--reload-config`、`--state-file`必須 | 待機中にSIGHUPを受けた時だけYAMLを再読込し、失敗時は直前の正常設定を維持して評価する。 |

## 4. Network Namespace Underlay

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-netns-setup.sh` | `sudo sh scripts/vm-netns-setup.sh` | なし | なし | `site-a`、`site-b`、`hub-1`、`relay-c` namespace と underlay link を作る。 |
| `scripts/vm-netns-smoke.sh` | `sudo sh scripts/vm-netns-smoke.sh` | なし | なし | direct、hub、relay のL3疎通をpingで確認する。 |
| `scripts/vm-netns-clean.sh` | `sudo sh scripts/vm-netns-clean.sh` | なし | なし | 作成した namespace を削除する。 |

## 5. IPsec Direct / Hub

IPsec操作は `scripts/vm-netns-ipsec.sh` を入口にします。

```sh
sh scripts/vm-netns-ipsec.sh direct generate
sudo sh scripts/vm-netns-ipsec.sh direct start
sudo sh scripts/vm-netns-ipsec.sh direct status
sh scripts/vm-netns-ipsec.sh direct logs
sudo sh scripts/vm-netns-ipsec.sh direct smoke
sudo sh scripts/vm-netns-ipsec.sh direct stop
sudo sh scripts/vm-netns-ipsec.sh direct clean
```

```sh
sh scripts/vm-netns-ipsec.sh hub generate
sudo sh scripts/vm-netns-ipsec.sh hub start
sudo sh scripts/vm-netns-ipsec.sh hub status
sh scripts/vm-netns-ipsec.sh hub logs
sudo sh scripts/vm-netns-ipsec.sh hub smoke
sudo sh scripts/vm-netns-ipsec.sh hub stop
sudo sh scripts/vm-netns-ipsec.sh hub clean
```

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-netns-ipsec.sh` | `sudo sh scripts/vm-netns-ipsec.sh hub status` | `<direct\|hub> <generate\|start\|status\|logs\|smoke\|stop\|clean>` | `RUN_BASE`, `SWANCTL_WORK_BASE` | direct/hub IPsec操作の共通入口。 |
| `scripts/vm-netns-ipsec-direct-generate.sh` | `sh scripts/vm-netns-ipsec-direct-generate.sh` | なし | `OUT_DIR`, `PSK` | direct用 `swanctl.conf` を生成する。通常は `vm-netns-ipsec.sh direct generate` 経由で使う。 |
| `scripts/vm-netns-ipsec-direct-start.sh` | `sudo sh scripts/vm-netns-ipsec-direct-start.sh` | なし | `OUT_DIR`, `RUN_BASE`, `SWANCTL_WORK_BASE`, `CHARON` | direct IPsec用charonをnamespace内で起動し、接続をload/initiateする。通常は `vm-netns-ipsec.sh direct start` 経由。 |
| `scripts/vm-netns-ipsec-direct-smoke.sh` | `sudo sh scripts/vm-netns-ipsec-direct-smoke.sh` | なし | `RUN_BASE` | direct IPsecのpingとESP counter増加を確認する。通常は `vm-netns-ipsec.sh direct smoke` 経由。 |
| `scripts/vm-netns-ipsec-direct-stop.sh` | `sudo sh scripts/vm-netns-ipsec-direct-stop.sh` | なし | `RUN_BASE`, `SWANCTL_WORK_BASE` | direct IPsec用charon停止とXFRM掃除。通常は `vm-netns-ipsec.sh direct stop` 経由。 |
| `scripts/vm-netns-ipsec-hub-generate.sh` | `sh scripts/vm-netns-ipsec-hub-generate.sh` | なし | `OUT_DIR`, `PSK` | hub用 `swanctl.conf` を生成する。通常は `vm-netns-ipsec.sh hub generate` 経由。 |
| `scripts/vm-netns-ipsec-hub-start.sh` | `sudo sh scripts/vm-netns-ipsec-hub-start.sh` | なし | `OUT_DIR`, `RUN_BASE`, `SWANCTL_WORK_BASE`, `DIRECT_RUN_BASE`, `CHARON` | hub IPsec用charonとXFRM interfaceを起動する。通常は `vm-netns-ipsec.sh hub start` 経由。 |
| `scripts/vm-netns-ipsec-hub-smoke.sh` | `sudo sh scripts/vm-netns-ipsec-hub-smoke.sh` | なし | `RUN_BASE` | hub IPsecのpingとESP counter増加を確認する。通常は `vm-netns-ipsec.sh hub smoke` 経由。 |
| `scripts/vm-netns-ipsec-hub-stop.sh` | `sudo sh scripts/vm-netns-ipsec-hub-stop.sh` | なし | `RUN_BASE`, `SWANCTL_WORK_BASE` | hub IPsec用charon停止、route/XFRM掃除。通常は `vm-netns-ipsec.sh hub stop` 経由。 |

## 6. VPP

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-vpp-preflight.sh` | `sh scripts/vm-vpp-preflight.sh` / `VPP_TOPOLOGY=hub sh scripts/vm-vpp-preflight.sh` | なし | `VPP_TOPOLOGY=edge\|hub` | VPP command/service/interface/FIB の状態を確認する。`hub`ではhub-1 namespaceとVPP per-instance config optionを診断するが、追加daemonは起動しない。 |
| `scripts/vm-install-vpp-fdio.sh` | `sudo DRY_RUN=0 sh scripts/vm-install-vpp-fdio.sh` | なし | `DRY_RUN=0\|1`, `CHANNEL`, `PACKAGECLOUD_SCRIPT_URL` | FD.io packagecloud repositoryを使ってVPPをinstallする補助。既定はdry-run。 |
| `scripts/vm-vpp-route-plan-smoke.sh` | `sh scripts/vm-vpp-route-plan-smoke.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/linux-vm-netns.yaml` | なし | directとhub fallbackに加え、`samples/vpp-vlan-hub-netns.yaml`のwaypoint VLAN sub-interface・ACL planをdry-runで確認する。 |
| `scripts/vm-relay-route-plan-smoke.sh` | `sh scripts/vm-relay-route-plan-smoke.sh samples/route-examples.yaml` | `[yaml]` | `BUILD_DIR`, `OUT_DIR` | Relay直列Pathのnode別route 4本が生成されることを確認する。実VPP forwardingではなくplan検証。 |
| `scripts/vm-vpp-netns-setup.sh` | `sudo sh scripts/vm-vpp-netns-setup.sh` | なし | なし | VPP host-interface と namespace veth を作る。 |
| `scripts/vm-vpp-netns-status.sh` | `sudo sh scripts/vm-vpp-netns-status.sh` | なし | なし | VPP interface/FIB とnetns側interfaceを表示する。 |
| `scripts/vm-vpp-netns-smoke.sh` | `sudo sh scripts/vm-vpp-netns-smoke.sh` | なし | なし | VPP経由で `site-a` / `site-b` のLAN疎通を確認する。 |
| `scripts/vm-vpp-netns-clean.sh` | `sudo sh scripts/vm-vpp-netns-clean.sh` | なし | なし | VPP netns接続用のveth/interfaceを削除する。 |
| `scripts/vm-vpp-controller-netns-smoke.sh` | `sudo sh scripts/vm-vpp-controller-netns-smoke.sh samples/linux-vm-netns.yaml` | `[yaml]` 省略時 `samples/linux-vm-netns.yaml` | `VPPCTL`, `VPPCTL_SOCKET` | controller-generated VPP netns route planを実VPPへ適用し、LAN疎通を確認する。生成planは`VPPCTL_SOCKET`指定時に`vppctl -s SOCKET`で接続する。 |
| `scripts/vm-vpp-routes-yaml-netns-smoke.sh` | `sudo sh scripts/vm-vpp-routes-yaml-netns-smoke.sh samples/vpp-netns-routes.yaml` | `[yaml]` 省略時 `samples/vpp-netns-routes.yaml` | `OUT_DIR` | YAML `routes[]`から生成したtable 100（VRF）の明示的な双方向VPP routeでLAN疎通を確認し、planのtable準備、VRF・各prefix・期待next-hopのVPP FIB出現も記録する。 |
| `scripts/vm-vpp-vlan-netns-smoke.sh` | `sudo VLAN_ID=100 VPP_TABLE_ID=100 sh scripts/vm-vpp-vlan-netns-smoke.sh` / `sudo VLAN_MATRIX="1 100 4094" sh scripts/vm-vpp-vlan-netns-smoke.sh` | なし | `VLAN_ID=1..4094`, `VLAN_MATRIX`, `VPP_TABLE_ID>=0` | Controller生成planを適用してLinux VLAN linkとVPP sub-interfaceを構成し、指定VRF tableへのroute投入、interface存在・タグ付きLAN疎通・指定VRF内の両方向prefix/期待next-hopのVPP FIB出現を確認する。親interfaceの実L3経路を使った未タグ通信がdeny ACLで拒否されることも確認する。`VLAN_MATRIX`指定時は各VLANを順番に再構成し、`matrix-summary.txt`へ結果と出力先を記録する。 |

## 7. Dependency Install

| script | 実行例 | 引数 | 主な環境変数 | 内容 |
| --- | --- | --- | --- | --- |
| `scripts/vm-install-deps-debian.sh` | `sudo sh scripts/vm-install-deps-debian.sh` | なし | なし | Debian/Ubuntu向けに基本依存をinstallする。 |

## 8. 推奨実行順

### 通常ユーザーで確認できる範囲

```sh
cd controller
sh scripts/vm-shell-check.sh
sh scripts/vm-build-cc.sh
sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml
sh scripts/vm-route-yaml-smoke.sh samples/route-examples.yaml
sh scripts/vm-evaluate.sh all samples/linux-vm-netns.yaml
sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

### Linux VMでruntimeまで確認

```sh
cd controller
sudo sh scripts/vm-netns-setup.sh
sudo sh scripts/vm-netns-ipsec.sh direct start
sudo sh scripts/vm-netns-ipsec.sh direct smoke
sudo sh scripts/vm-netns-ipsec.sh direct stop
sudo sh scripts/vm-netns-ipsec.sh hub start
sudo sh scripts/vm-netns-ipsec.sh hub smoke
sudo sh scripts/vm-netns-ipsec.sh hub stop
```

### VPPまで確認

```sh
cd controller
sh scripts/vm-vpp-preflight.sh
sudo sh scripts/vm-vpp-netns-setup.sh
sudo sh scripts/vm-vpp-controller-netns-smoke.sh samples/linux-vm-netns.yaml
sudo sh scripts/vm-vpp-routes-yaml-netns-smoke.sh samples/vpp-netns-routes.yaml
sudo RUN_RUNTIME=1 sh scripts/vm-evaluate.sh all samples/linux-vm-netns.yaml
```

## 9. 評価レポート生成

`scripts/vm-evaluate.sh` は、実装方針の「今後の評価で確認すべき点」を実行可能な確認項目に分解した入口です。

```sh
cd controller
sh scripts/vm-evaluate.sh all samples/linux-vm-netns.yaml
```

出力は既定で `out/evaluation/YYYYMMDD-HHMMSS/` に作られます。`summary.md`の末尾には`pass`、`partial`、`skip`、`fail`の件数も追記され、`environment.txt`にはHEADとの差分SHA-256も保存されます。

| case | 実行例 | 内容 |
| --- | --- | --- |
| `scenario` | `sh scripts/vm-evaluate.sh scenario samples/linux-vm-netns.yaml` | direct選択、direct障害時fallback、評価値によるrelay選択、multi-step状態遷移を確認する。 |
| `fallback-time` | `EVAL_REPEAT=10 sh scripts/vm-evaluate.sh fallback-time samples/linux-vm-netns.yaml` | Direct障害からfallback runtime plan生成までの時間を複数回測る。 |
| `explain` | `sh scripts/vm-evaluate.sh explain samples/linux-vm-netns.yaml` | Explain JSONLから選択pathと判断結果を追跡できるか確認する。 |
| `threshold` | `sh scripts/vm-evaluate.sh threshold samples/linux-vm-netns.yaml` | Agentの連続測定値とthreshold設定によるPath維持、fallback、recoveryを確認する。 |
| `xfrm-policy` | `sh scripts/vm-evaluate.sh xfrm-policy samples/linux-vm-netns.yaml` | `block_non_ipsec`によるselector限定のXFRM block plan生成を確認する。 |
| `xfrm-runtime` | `sudo sh scripts/vm-evaluate.sh xfrm-runtime samples/linux-vm-netns.yaml` | namespaceへselector限定のXFRM blockを実適用し、policy一覧・priority・cleanupを確認する。 |
| `xfrm-cleartext` | `sudo sh scripts/vm-evaluate.sh xfrm-cleartext samples/linux-vm-netns.yaml` | direct IPsec暗号化疎通、block policy併用時の疎通、SA停止後のcleartext遮断を確認する。`RUN_RUNTIME=1`の`all`にも含まれる。 |
| `integrated-direct` | `sudo sh scripts/vm-evaluate.sh integrated-direct samples/linux-vm-netns.yaml` | controller-generated planでIPsec direct pathとVPP forwardingを通す。 |
| `integrated-fallback` | `sudo sh scripts/vm-evaluate.sh integrated-fallback samples/linux-vm-netns.yaml` | direct障害eventからhub fallbackを選び、IPsec/VPP runtimeで疎通する。 |
| `transition-policy` | `sh scripts/vm-evaluate.sh transition-policy samples/linux-vm-netns.yaml` | Immediate/Gracefulの切替・draining・cleanup・rollbackをCテストで確認し、Flow Preserveはpartialとして記録する。 |
| `rollback` | `sh scripts/vm-evaluate.sh rollback samples/linux-vm-netns.yaml` | rollbackに必要なfallback planと、選択runtimeを撤去する`rollback-selected.sh`生成を確認する。実際の失敗注入からの自動rollbackは`rollback-runtime`で検証する。 |
| `rollback-runtime` | `sudo sh scripts/vm-evaluate.sh rollback-runtime samples/linux-vm-netns.yaml` | direct runtimeへテスト失敗を注入し、自動rollbackでcharon／XFRM stateが撤去されることを確認する。 |
| `control-plane` | `sh scripts/vm-evaluate.sh control-plane samples/linux-vm-netns.yaml` | eventnetdのファイル入力・Unix socket入力によるfailure/recovery反映を確認する。 |
| `restart-recovery` | `sh scripts/vm-evaluate.sh restart-recovery samples/linux-vm-netns.yaml` | eventnetd再起動後のstate復元と、Linuxでsymlink／危険なmodeのstate fileを拒否することを確認する。 |
| `state-boundary` | `sh scripts/vm-evaluate.sh state-boundary samples/linux-vm-netns.yaml` | 別Intentのtraffic key、explicit Intentの候補外Path、disabled Node経由Pathをstate fileから復元しないことをCTestで確認する。 |
| `backend-reuse` | `sh scripts/vm-evaluate.sh backend-reuse samples/linux-vm-netns.yaml` | strongSwan／VPPの公開adapter契約、同じ選択coreからのswanctl計画とVPP/netns計画生成を確認する。 |
| `federation` | `sh scripts/vm-evaluate.sh federation samples/linux-vm-netns.yaml` | 複数Domain Controller間のTransition Proposalが未実装であることを評価レポートに残す。 |
| `plan-secret-permission` | `sh scripts/vm-evaluate.sh plan-secret-permission samples/ipsec-routes.yaml` | Linuxで生成swanctl.confの0600権限とsymlink経由上書き拒否を評価レポートへ記録する。 |
