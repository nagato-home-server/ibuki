# 論文作成から未踏期間までの実装・評価計画

## 目的

論文作成までに、Ibukiの中心提案である「ユーザがYAMLで定義したPolicyとAgentの実測telemetryに基づき、複数のIPsec Pathから経路を選択し、VPP forwardingを状態遷移させる」ための再現可能な土台を完成させる。論文作成後から未踏期間までは、土台を壊さずに実ネットワークで閾値と通信影響を測定し、実装の有効性を示す。

## 論文作成まで

論文作成開始時点では、新しい機能を広げるよりも、現在のPrototypeの入力・判断・出力を固定する。YAMLでは`failure_threshold`、`recovery_threshold`、`hold_down_ms`、`hysteresis_percent`をユーザが変更できるようにし、Controllerが値を検証してPath Selectionへ渡す。Agentは閾値を判断せず、RTT、Packet Loss、Jitter、連続成功・失敗回数をtelemetryとして出力する。これにより、同じ実測値へ異なるPolicyを適用した比較実験が可能になる。

この段階で完成させる範囲は、Direct／Hub／RelayのYAML route表現、Explicit／Priority／Evaluated selection、Failure／Recovery、Immediate／Gracefulの状態遷移、rollback、eventnetdの周期入力・socket入力・設定reload・state復元、strongSwan IPsec、VPP CLIのroute／VLAN／VRF／FIB反映、Explain JSONL、評価manifestである。VICIとVPP Binary APIは接続境界を維持し、対象SDK版に依存する具体codecは未完成として明示する。

論文用の合格条件は、Linux VMで同一YAMLと同一telemetryから同じPath選択結果を得られること、Direct障害時にHubまたはRelayへ切り替えられること、ESP counterとVPP forwardingを確認できること、設定変更・不正設定・古いtelemetryを評価できることである。評価結果は`pass`、`partial`、`skip`、`fail`を混同せず、実験環境と作業ツリーの情報を保存する。

### 2026-09-25進捗

- 制御層: CTest 27件がpassし、Path選択、failure/recovery、設定検証、telemetry、Graceful/rollbackの制御ロジックまで実装済み。
- 実データパス: strongSwan/XFRM + VPP GREと、比較用VPP Native IPIP/IPsecで双方向疎通、暗号counter、cleanup、再適用を確認済み。
- 論文本文: 章立てと初期結果は記述済み。旧評価値を現在参照できる証拠へ置き換えた。
- 未完了: 反復した定量測定、工程別時間、最大通信断、RTT/reordering/TCP retransmission、CPU/メモリ、図表生成、提出commitでのLinux/Windows最終再現、PDF校正。

したがって、現在の主な不足は新しいBackendの追加ではなく、既存機能を同一条件で測って論文の主張へ結び付ける評価工程である。

## 論文作成後から未踏開始まで：実験環境

最初に、Linux namespaceで再現できる経路を、物理機または小規模クラウドへ移す。最低限、site-a、site-b、hubまたはrelayの3拠点相当を用意し、各区間で遅延、Packet Loss、Jitter、帯域を独立に変更できるようにする。高価な専用測定器を必須にせず、`tc netem`、`iperf3`、`ping`、`tcpdump`、VPP統計を組み合わせ、同じ条件を再実行できる構成を優先する。

この段階では、クラウドを常時稼働させる必要はない。異なる地域間のWAN条件やInternet越しIPsecを確認するときだけ小容量VMを起動し、実験条件、課金時間、endpoint、取得telemetryを保存する。物理環境を使える場合はVLAN対応スイッチと複数NICを用意し、タグ付き通信と未タグ通信を分離して検証する。

## 閾値決定実験

次に、閾値を固定値として主張せず、複数のPolicyを同じ障害シナリオへ適用する。Packet Loss、連続失敗回数、連続成功回数、hold-down、hysteresisを変数にして、障害注入から切替完了までの時間、不要な切替回数、ping loss、TCP retransmission、UDP loss、旧Tunnel cleanup時間を記録する。

実験後は、ユーザがYAMLを変更できることと、変更結果がExplain JSONLへ反映されることを確認する。既定値を設ける場合も、既定値を最適値とは扱わず、環境ごとにPolicyを調整できる設計として説明する。閾値変更時は設定reloadの成否、現在のActive Path、既存stateの扱いをログに残し、不正な閾値では旧設定を維持する。

## 未踏期間の実装優先順位

未踏開始直前には、YAML schema、telemetry schema、Controller adapter API、評価scriptを凍結する。未踏期間の最優先実装は、VPP Binary APIの対象バージョンを決めたうえでのroute／VRF／VLAN codecと、実通信でのGraceful Transition評価である。これらは既存のPath Selection coreを変更せず、backendと評価層を拡張する。

次に、FRR／BGPをLinux namespaceで接続し、Ibukiが選択したPathを外部routeへ広告し、障害時のwithdrawalと再広告を確認する。Controller HAは2つのController process、state store、leader／standby、active Path維持、引き継ぎ後のreconcileを最小構成で実装する。Flow PreserveはTCP長時間通信を使い、既存flowと新規flowの分類を導入できる見通しが立った場合に着手する。

## 未踏期間中に後回しにするもの

GUI、永続database、クラウド固有VPN API、完全な複数Domain federation、Hardware offload、全てのVPP API version対応は後段とする。これらを先に行うと、閾値とPath Transitionの有効性を測る時間が減るためである。未実装機能は評価レポート上で`partial`または`skip`として残し、論文の中核結果と混同しない。

## 期間ごとの成果物

| 期間 | 成果物 |
| --- | --- |
| 論文作成まで | C実装、YAML／telemetry schema、VM smoke、Explain、評価manifest、既知の制限 |
| 論文後〜未踏前半 | 実験環境、障害注入手順、実測telemetry、閾値比較表 |
| 未踏前半〜中盤 | Graceful定量評価、VPP Binary API codec、VLAN／VRF実反映 |
| 未踏中盤〜後半 | FRR／BGP、Controller HA、必要に応じたFlow Preserve |
| 未踏終了前 | 再現実験、成果グラフ、性能限界、未実装範囲、提出用デモ |
