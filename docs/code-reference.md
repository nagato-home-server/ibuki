# Ibuki Cコード関数リファレンス

## 1. 目的と読み方

この文書は、IbukiのC実装を「どの関数が、どの入力を受け、どの状態または外部ネットワークを変更するか」という単位で整理したコードリファレンスである。設計書の用語と実装の対応を確認するときは、次の順に読む。

1. `include/eventnet/types.h` でデータ構造と固定長配列の上限を確認する。
2. `src/yaml_config.c` でYAMLを構造体へ変換する流れを確認する。
3. `src/path_selection.c`、`src/transition.c` で選択と状態遷移を確認する。
4. `src/command_adapters.c`、`src/strongswan_vici_adapter.c`、`src/vpp_api_adapter.c` で外部実装への接合を確認する。
5. `examples/eventnetd.c` または `examples/netns_plan.c` で実行入口を確認する。

本リファレンスは、論文提出時点の実装範囲と、将来の本番化範囲を区別する。`mock`、`dry-run`、シェル計画生成は検証用であり、実機接続を意味しない。

## 2. 全体の呼び出し経路

```text
YAML / JSONL
  -> en_yaml_config_load_file / en_telemetry_load_jsonl
  -> en_controller_t
  -> en_select_path
  -> en_transition_path
  -> strongSwan adapter + VPP adapter
  -> state / audit / JSON status
```

実プロセスでは `eventnetd` が設定を読み込み、Telemetryを反映し、イベントまたは周期処理からreconcileを呼び出す。計画生成だけを行う場合は `netns_plan` が同じ構造体を読み、strongSwan設定、VPP/GRE計画、適用・rollbackスクリプトを出力する。

## 3. データモデル

### `include/eventnet/types.h`

| 要素 | 役割 |
|---|---|
| `en_node_t` | 拠点、Hub、Relayの識別子・管理状態・能力を保持する。 |
| `en_segment_t` | Path中の隣接ノード間リンクと、対応するTunnelを表す。 |
| `en_path_t` | sourceからdestinationまでの候補経路、waypoint、segment、優先度、運用状態を保持する。 |
| `en_tunnel_t` | strongSwan CHILD、GRE、VTI等のトンネル属性を保持する。 |
| `en_intent_t` | 通信元・宛先、Path選択方式、制約、遷移ポリシー、VLAN/VRF条件を保持する。 |
| `en_path_health_t` | RTT、loss、probe時刻、健全性、観測元を保持する。 |
| `en_controller_t` | YAML状態、Path状態、health、適用済みPath、adapter、監査履歴を束ねる。実体は非公開で、操作はAPI経由で行う。 |
| `en_vpp_edge_t` | ノードとVPPの接続情報。`vpp_socket` は名前空間内VPPのCLI/Binary API接続先である。 |
| `en_reconcile_result_t` | 選択Path、理由、除外理由、遷移状態、エラーを返す。 |

配列は現在、`EN_MAX_NODES`、`EN_MAX_PATHS`、`EN_MAX_TUNNELS`、`EN_MAX_INTENTS`、`EN_MAX_SEGMENTS` 等の固定上限で管理する。入力上限を超える場合は読み込みまたは検証でエラーにする。動的な大規模管理へ移行する場合は、構造体の所有権、解放順序、参照の無効化を設計し直す必要がある。

## 4. コアライブラリ

### `src/state.c`

| 関数 | 入力・出力 | 処理と副作用 |
|---|---|---|
| `en_now_ms` | なし -> `long long` | 単調時計の現在時刻を返す。hold-down等の時間判定に使う。 |
| `en_copy_id` | 文字列 -> 固定長バッファ | IDを安全にコピーし、終端を保証する。 |
| `en_streq` | 2文字列 -> `bool` | NULLを含む識別子比較を行う。 |
| `en_path_hop_count` | Path -> hop数 | segment/waypointから経路長を計算する。 |
| `en_find_path` | Controller, Path ID -> mutable Path | 内部状態を変更する処理用の検索関数。 |
| `en_find_health` | Controller, Path ID -> health | Pathの最新測定値を検索する。 |
| `en_find_tunnel` | Controller, Tunnel ID -> mutable Tunnel | 設定済みTunnelを検索する。 |
| `en_find_desired_tunnel` | Controller, Tunnel ID -> desired Tunnel | reconcile対象の望ましいTunnelを検索する。 |
| `path_nodes_enabled` | Controller, Path -> `bool` | Path上の全ノードが管理上有効か判定する。 |
| `en_controller_find_path` | const Controller, Path ID -> const Path | 外部参照用の読み取り専用検索API。 |
| `en_controller_path_nodes_enabled` | const Controller, Path -> `bool` | 外部からノード有効性を確認するAPI。 |
| `en_find_node` | const Controller, Node ID -> const Node | ノード検索を行う。 |
| `en_get_applied_path` | Controller, traffic key -> Path ID | 現在適用済みのPathを返す。 |
| `en_get_applied_since_ms` | Controller, traffic key -> 時刻 | 適用開始時刻を返す。 |
| `en_set_applied_path` | Controller, traffic key, Path ID | 適用済みPathを更新し、時刻も更新する。 |
| `en_controller_restore_applied_path` | Controller, traffic key, Path ID | 再起動復旧用に適用Pathを復元する。 |
| `en_make_traffic_key` | Traffic selector -> バッファ | source/destination/VLAN/VRFを衝突しないキーへ変換する。 |
| `en_controller_applied_path` | const Controller, traffic key -> Path ID | 読み取り専用の適用Path参照API。 |
| `en_health_state_name` | health enum -> 文字列 | JSON・ログ向けの状態名を返す。 |
| `en_transition_state_name` | transition enum -> 文字列 | 遷移状態名を返す。 |
| `en_error_code_name` | error enum -> 文字列 | エラーコード名を返す。 |

### `src/path_selection.c`

| 関数 | 役割 |
|---|---|
| `en_select_path` | Intentの候補を走査し、制約・健全性・ノード状態・比較順に従ってPathを選ぶ中心API。選択理由と除外理由を結果に記録する。 |
| `exclusion_reason` | 候補を除外した理由を、failed、stale、制約違反、hold-down等に分類する。 |
| `node_has_capability` | ノードが要求された能力を持つか確認する。 |
| `path_nodes_enabled` | Path上のノードが全て有効か確認する。 |
| `path_supports_capabilities` | Intentの要求能力をPath全体が満たすか確認する。 |
| `waypoint_contains` | Pathのwaypointに指定ノードが含まれるか確認する。 |
| `compare_paths` | `packet_loss`、`rtt`、hop数、administrative priority等の比較順で二候補を比較する。 |
| `metric_value` | 比較キーに対応する数値をPath/healthから取り出す。 |

`explicit` は指定Pathのみ、`priority` は優先度順、`evaluated` は制約を満たす候補を比較順で選択する。比較値が未観測の場合は候補を安全側に除外し、全候補が使えない場合はエラー結果にする。

### `src/transition.c`

| 関数 | 役割 |
|---|---|
| `en_transition_path` | 選択結果を現在の適用状態へ反映する統合API。prepare、forwarding、commit、rollbackの境界を管理する。 |
| `sleep_ms` | retry、drain、hold-down待機用の内部待機。 |
| `transition_failed` | 失敗結果を遷移状態とエラーへ変換する。 |
| `rollback_previous_path` | 旧Pathの復元操作をadapterへ依頼する。 |
| `remove_previous_path` | 新Path確立後に旧Pathを削除する。 |
| `apply_target_path` | Tunnel確立と転送経路適用を順序付ける。 |
| `drain_previous_path` | graceful設定時の旧Path排出待ちを行う簡易実装。Flow Preserveの完全なフロー追跡ではない。 |
| `record_transition` | 監査履歴へ遷移結果を書き込む。 |

Immediateは直ちに切替、Gracefulは簡易drainと待機を伴う。Flow Preserveは設計上の予約であり、現在のC実装はフロー単位の移行を保証しない。

### `src/controller.c`

| 関数 | 役割 |
|---|---|
| `en_controller_create` | 設定、adapter、初期Path状態を組み合わせてControllerを初期化する。 |
| `en_controller_destroy` | Controllerが所有する履歴・状態を破棄する。 |
| `store_health` | 最新healthをPath ID単位で保存する。 |
| `observe_one` | 1 Pathのprobeをhealth adapterへ依頼し、結果を保存する。 |
| `observe_candidate_health` | Intent候補のhealthを収集する。 |
| `en_controller_create` / `en_controller_create_with_tunnels` / `en_controller_create_with_nodes_and_tunnels` | Pathだけ、Tunnel付き、Node/Tunnel付きの3形式でControllerを初期化する。 |
| `en_controller_submit_intent` | health観測、Path選択、必要な遷移を一連で実行するreconcile入口。 |
| `en_controller_audit_events` / `en_controller_errors` | 監査イベントとエラー履歴を読み取り専用で取得する。 |

## 5. 設定・Telemetry・出力

### `src/yaml_config.c`

| 関数 | 役割 |
|---|---|
| `en_yaml_config_load_file` | YAMLファイルを行単位で読み込み、Path/Tunnel/Intent/Node/VPP edgeへ変換する入口。 |
| `en_yaml_config_validate` | ID、参照関係、アドレス、selector、VLAN/VRF、遷移設定、VPP socketを検証する。 |
| `parse_line` | YAMLのインデントとkey/valueを状態機械へ渡す。 |
| `parse_*_kv` | Tunnel、Node、Path、Segment、Route、Intent、VPP edge、selection、constraints、transition、fallbackの値を各構造体へ設定する。 |
| `copy_id` / `copy_value` | 固定長構造体へ安全に値をコピーする。 |
| `valid_config_token` | shell・設定生成へ渡してよい文字だけか確認する。 |
| `set_error` | 行番号付きエラーをバッファへ記録する。 |
| `validate_*` 系関数 | Path、Tunnel、Intent、VLAN、能力、参照整合性を個別に検証する。 |

YAMLは汎用YAML仕様全体ではなく、Ibukiが定義したサブセットを対象とする。Web UIを追加する場合も、UI入力を同じ内部構造体と検証関数へ通す設計にする。

### `src/telemetry.c`

| 関数 | 役割 |
|---|---|
| `en_telemetry_open_jsonl` | JSONL出力ファイルを安全なモードで開く。 |
| `en_telemetry_parse_json_line` | yyjsonで1行を解析し、Path healthまたはイベントへ変換する。必須項目、型、範囲、識別子を検証する。 |
| `en_telemetry_load_jsonl` | ファイルを逐次読み込み、容量内のhealth配列へ格納する。時刻や順序の不正を処理する。 |
| `valid_label` / `set_error` | JSONLの識別子検証とエラー記録を補助する。 |

JSON処理はyyjsonへ統一している。外部DBは現在必須ではなく、長期履歴、集計、複数Controller共有が必要になった段階でTelemetry収集側に追加する。

### `src/json_output.c`

| 関数 | 役割 |
|---|---|
| `en_json_mut_doc_write_line` | yyjson mutable documentを1行のJSONLとして出力し、改行とflush境界を管理する。 |
| `en_json_mut_doc_to_buffer` | yyjson mutable documentを終端付き文字列へシリアライズする。 |

### `src/audit.c`

| 関数 | 役割 |
|---|---|
| `en_audit_append` | Controllerの監査履歴へイベントを追記する。 |
| `en_error_append` | エラー履歴へコード、メッセージ、時刻を追記する。 |
| `audit_*` 内部関数 | 固定長履歴の空き・上限・JSON出力用整形を処理する。 |

## 6. 外部接続アダプタ

### `src/command_adapters.c`

| 関数 | 実装範囲 |
|---|---|
| `en_strongswan_command_adapter` | strongSwan操作をコマンドテンプレートへ接続する。dry-runと実行を切り替える。 |
| `en_vpp_command_adapter` | VPP操作をCLIコマンドへ接続する。経路、GRE、VLAN、VRF、ACLを扱う。 |
| `en_health_command_probe` | ping等の外部probe結果をhealthへ変換する。 |
| `strongswan_ensure` / `strongswan_remove` | CHILD SA作成・削除を実行し、必要ならXFRM状態を確認する。 |
| `vpp_install` / `vpp_remove` | PathのVPP forwardingを適用・撤去する。 |
| `vpp_active_path` | 現在のVPP経路から適用Pathを観測する。 |
| `health_validate` | コマンドprobe結果の妥当性を検査する。 |
| `run_template` |安全な変数置換後にコマンドを実行する。 |
| `run_shell_command` | 互換用shell境界。入力検査済みの計画に限定して使う。 |
| `run_exec_command` / `run_exec_capture` | shellを介さない外部コマンド実行・出力取得を行う。 |
| `apply_xfrm_block` / `remove_xfrm_block` | IPsec対象外通信を遮断するXFRM policyの適用・撤去を行う。 |
| `xfrm_policy_exists` / `has_xfrm_block` | XFRM状態の存在確認を行う。 |
| `verify_vpp_route` | 期待next-hop、interface、tableのVPP経路を確認する。 |
| `capture_vpp_command` | VPP CLI出力を取得する。 |
| `verify_vpp_vlan_interfaces` | VLAN sub-interfaceの存在と状態を確認する。 |
| `ensure_vpp_vlan_interfaces` | VLAN sub-interfaceを作成しupにする。 |
| `ensure_vpp_vlan_interface_tables` | VLANをVRF/FIB tableへ割り当てる。 |
| `ensure_vpp_unmatched_vlan_acl` / `remove_vpp_unmatched_vlan_acl` | 許可されないVLANの既定拒否ACLを管理する。 |
| `vpp_vlan_edge_is_shared` / `vpp_edge_used_by_path` | 共有edgeの重複作成・早期削除を防ぐ。 |

### `src/strongswan_vici_adapter.c` と `src/strongswan_vici_client.c`

| 関数 | 役割 |
|---|---|
| `en_strongswan_vici_adapter` | VICI clientをIbukiのstrongSwan adapter interfaceへ束ねる。 |
| `en_strongswan_vici_bind_client` | VICI clientの関数テーブルをcontextへ結び付ける。 |
| `en_strongswan_vici_observe_tunnel` | CHILD SA状態をVICIから取得し、観測構造体へ変換する。 |
| `en_strongswan_vici_observe_event_json` | VICIイベントをIbuki JSONLイベントへ変換する。 |
| `ensure_tunnel` / `remove_tunnel` | VICI操作のadapter callback。 |
| `client_ensure_tunnel` / `client_remove_tunnel` / `client_observe_tunnel` | client実装へ委譲するcallback。 |
| `en_strongswan_vici_client_connect` | VICI socketへ接続する。 |
| `en_strongswan_vici_client_request` | requestを送信しresponseを受け取る。 |
| `en_strongswan_vici_client_listen` | eventを受信しcallbackへ渡す。 |
| `en_strongswan_vici_client_close` | socket・受信バッファ・client状態を閉じる。 |

VICIの本番接続点は実装済みだが、証明書配置、鍵更新、権限分離、複数namespaceのdaemon監視は運用層の追加課題である。

### `src/vpp_api_adapter.c` と `src/vpp_api_transport.c`

| 関数 | 役割 |
|---|---|
| `en_vpp_api_adapter` | Binary API transportをVPP adapterへ束ねる。 |
| `en_vpp_api_message_adapter` | message callback型のVPP実装をadapter interfaceへ変換する。 |
| `en_vpp_api_observe_route` | VPP FIBから経路観測を行う。 |
| `en_vpp_api_observe_interface` | VPP interface状態を観測する。 |
| `en_vpp_api_transport_init` | transportのfd、接続状態、message tableを初期化する。 |
| `en_vpp_api_transport_dispatch` |受信messageをdispatchしcallbackを呼ぶ。 |
| `en_vpp_api_transport_alloc_message` | VPP message領域を確保する。 |
| `en_vpp_api_transport_send_message` | messageを送信する。 |
| `en_vpp_api_transport_free_message` | message領域を解放する。 |
| `en_vpp_api_transport_is_message_available` | message IDの登録・受信可否を確認する。 |
| `en_vpp_api_transport_get_fd` | event loopへ渡すfdを返す。 |
| `en_vpp_api_transport_close` | Binary API transportを閉じる。 |
| `en_vpp_api_transport_is_connected` | 接続状態を返す。 |

現行の名前空間VPP runtimeはCLI socketを計画生成から利用できる。Binary APIは接続抽象化とtransport境界まであり、実VPP message定義をリンクした本番操作は次段階である。

### モック・観測・描画

| ファイル | 関数群 | 役割 |
|---|---|---|
| `src/strongswan_adapter_mock.c` | `en_strongswan_mock_adapter`、`ensure_tunnel`、`remove_tunnel` | SA操作をメモリ内で再現する。 |
| `src/vpp_adapter_mock.c` | `en_vpp_mock_adapter`、route/VLAN/VRF操作群 | VPP適用順序とrollbackを記録する。 |
| `src/health_probe.c` | `en_health_probe_mock_adapter`、`en_health_probe_mock_set`、`validate_path` | probe結果を注入する。 |
| `src/strongswan_observer.c` | `child_header`、`state_value`、`map_state` | `swanctl --list-sas`等の出力を観測構造体へ変換する。 |
| `src/vpp_observer.c` | `route_header`、`token_after`、`has_word` | `show ip fib`等の出力をVPP観測へ変換する。 |
| `src/render_commands.c` | `en_render_vpp_gre_*`、token/selector検証群 | VPP GRE作成・削除・アドレス・MTU・up・経路コマンドを生成する。 |
| `src/topology.c` | `en_initial_demo_paths`、`set_segment` | 最小デモ用トポロジを構築する。 |

## 7. 実行プログラム

### `examples/eventnetd.c`

| 関数 | 役割 |
|---|---|
| `main` | daemon入口。設定、adapter、signal、Telemetry、reconcile loopを初期化する。 |
| `handle_reload_signal` / `handle_shutdown_signal` | SIGHUP reload、SIGTERM/SIGINT終了を要求する。 |
| `install_signal_handler` / `shutdown_requested` | signal状態を管理する。 |
| `find_intent` / `intent_references_path` | 対象IntentとPath参照を検索する。 |
| `load_health` | JSONL healthをidentityと鮮度付きでControllerへ反映する。 |
| `telemetry_edge_allows_vlan` / `telemetry_edge_belongs_to_path` / `telemetry_interface_matches_path` | 外部Telemetryが対象Pathのものか確認する。 |
| `telemetry_identity_matches` | spoofされたnode/path/interface/VLAN記録を拒否する。 |
| `restore_state` / `persist_state` | 再起動時の適用Pathを保存・復元する。 |
| `write_status_json` / `open_status_output` | 状態をyyjson JSONL/JSONへ出力する。 |
| `eventnetd_vici_should_stop` / `eventnetd_vici_event` | VICI event loopとdaemon終了を接続する。 |
| `valid_*_argument` / `parse_*_argument` | CLI引数と状態ファイルの値を検証する。 |

### `examples/netns_plan.c`

| 関数 | 役割 |
|---|---|
| `main` | YAMLからruntime成果物を生成する。 |
| `write_swanctl_plan` | TunnelごとのstrongSwan設定を出力する。 |
| `write_vpp_route_plan` | rootまたはsocket指定VPPの経路/GRE計画を出力する。 |
| `write_vpp_netns_route_plan` | namespace内VPP向けの計画を出力する。 |
| `write_vpp_node_dispatch` | node IDからVPP CLI socketを選び、`run_vpp_node`を生成する。 |
| `write_apply_script` / `write_integrated_script` | IPsec、VPP、GREを適用する入口を出力する。 |
| `write_rollback_script` | 生成済み経路・Tunnelを逆順で撤去する。 |
| `write_summary` | 選択Path、runtime kind、segment、適用/rollbackを説明する。 |
| `path_uses_namespaced_vpp` | Pathがsocket指定VPPを使うか判定する。 |
| `path_has_gre_tunnel` / `find_first_gre_tunnel` | GRE計画の有無と対象Tunnelを検索する。 |
| `find_path` / `find_tunnel` / `find_intent` / `find_vpp_edge` | YAML構造体内の要素を検索する。 |
| `vpp_edge_allows_vlan` / `path_has_conflicting_tables` / `path_uses_node` | VLAN、VRF、Path構造の生成前検査を行う。 |
| `ensure_directory_tree` |出力ディレクトリを作成する。 |

### その他のexamples

| 実行ファイル | 主な関数 | 目的 |
|---|---|---|
| `eventnet_scenario` | `run_scenario`、`apply_named_step`、`append_health`、`generate_runtime` | priority/evaluated/fallback/recoveryを再現する。 |
| `eventnet_agent` | `measure_ping`、`parse_ping_rtt`、`open_agent_output` | 拠点間のRTT/lossを測定してJSONLへ出力する。 |
| `strongswan_vici_controller_probe` | `monitor_event`、`find_intent` | VICI接続とevent観測を確認する。 |
| `vpp_api_transport_probe` | `main`、`usage` | Binary API transportの接続境界を確認する。 |
| `swanctl_observer` | `main` | strongSwan観測parserを単独確認する。 |
| `vpp_observer` / `vpp_interface_observer` | `main`、parser群 | VPP route/interface観測を単独確認する。 |
| `demo` | `make_demo_intent`、`main` | 最小Controller APIの使用例を示す。 |

## 8. シェル実行境界

| スクリプト群 | 関数・入口 | 責務 |
|---|---|---|
| `vm-vpp-ns-runtime.sh` | `start`、`stop`、`status` | site namespace内でVPP process、CLI socket、API socket、pid/logを分離する。 |
| `vm-vpp-ns-topology.sh` | `setup`、`clean` | LAN/underlay veth、VPP host-interface、underlay routeを構築・撤去する。 |
| `vm-netns-ipsec-gre-start.sh` | `start` | site namespace内charonとVICI socketを起動する。 |
| `vm-gre-namespace-v2-smoke.sh` | main sequence | topology、strongSwan、生成計画、VPP GRE、双方向pingを統合検証する。 |
| `vm-build-cc.sh` | build stages | C23/yyjsonを含むC実装をコンパイルし、unit testを実行する。 |
| `vm-paper-validation.sh` | case dispatcher | 論文用のunit、scenario、Telemetry、policy、runtime評価を分類実行する。 |

シェルは制御ロジック本体ではなく、Linux namespace、strongSwan、VPPという実行環境を用意する検証・運用境界である。入力値は生成器で検証し、スクリプト自身も`sh -n`で構文検査する。

## 9. テスト関数対応表

`tests/test_controller.c` のテストは、次の機能群に対応する。

| テスト関数群 | 確認対象 |
|---|---|
| `test_priority_*`、`test_evaluated_*` | Path選択、除外理由、品質比較 |
| `test_failed_forwarding_*`、`test_successful_switch_*` | rollback、旧Path削除、retry |
| `test_graceful_switch_*`、`test_hold_down_*`、`test_path_selection_thresholds_*` | graceful、flap抑制、閾値 |
| `test_yaml_*` | YAML各形式、参照整合性、VLAN/VRF、能力、危険値拒否 |
| `test_telemetry_*`、`test_path_events_*` | JSONL schema、順序、health/event変換 |
| `test_command_adapter_*` | dry-runコマンド、XFRM、VPP route、VLAN ACLの適用順序 |
| `test_swanctl_*`、`test_vpp_*observer*` | 外部CLI観測parser |
| `test_vpp_api_*`、`test_strongswan_vici_*` | adapter callback、transport lifecycle |
| `test_renderers_*` | strongSwan/VPP/GRE生成結果 |
| `test_*security*` | secret、selector、token、path traversal、shell injection拒否 |

テストは外部ネットワークを変更しないモック中心であり、Linux VMの実疎通は`vm-paper-validation.sh`および各smoke scriptで別に確認する。

## 10. 実装状態と残課題

### 論文提出に利用できる実装

- Explicit/Priority/Evaluated Path選択
- priority fallback、health、threshold、hold-down、簡易Graceful
- YAMLサブセットの検証と複数route形式
- yyjsonによるJSON/JSONL入出力
- strongSwan command/VICI adapter境界
- VPP command adapter、観測parser、Binary API transport抽象化
- root VPP互換構成と、`vpp_socket`を用いるnamespace VPP構成
- namespace内のLAN/underlay veth、GRE over IPsecの生成計画
- apply/rollback計画、VLAN/VRF/ACLのdry-run検証

### 未完了または実機依存の項目

- Linux VM上でのnamespace VPP + strongSwan + GRE暗号化双方向pingの反復確認
- 実VPP Binary API message定義を用いたroute/interface操作
- Agentの並列測定、再送・タイムアウト・証明書付きTelemetry transport
- 実環境での証明書認証、鍵更新、HA、BGP/FRR
- Flow Preserveのフロー単位移行
- 固定長配列を超える規模への動的管理
- 外部Telemetry DBと長期集計

したがって、「関数単位の責務説明」はこの文書で整備済みだが、上記の未完了項目を実装済みと誤認してはならない。特にVPP Binary APIは接続境界まで、名前空間GREは生成・統合入口までが現在の範囲である。

## 11. 変更時の更新規則

新しい公開関数を追加した場合は、対応するヘッダの節、呼び出し経路、テスト関数、外部副作用を同じ変更で更新する。新しいYAML key、CLI option、shell entry pointを追加した場合は、`docs/yaml-routes.md`、`docs/shell-commands.md`、本書の対応表を更新する。外部接続を実装した場合は、mock、dry-run、実環境smokeの三段階を区別して記録する。
