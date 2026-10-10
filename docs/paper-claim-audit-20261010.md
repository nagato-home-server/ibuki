# 研究原稿と実装・測定結果の照合

対象は `研究内容.tex`、2026-10-10の作業ツリー。論文の完成条件は完成製品ではなく、主張の範囲と証拠の一致とする。参考文献の内容・新規性を外部資料と再検証する作業、LuaLaTeXでの組版確認は今回の照合に含まない。

## 主張ごとの証拠と境界

| 原稿の対象 | コード・結果 | 原稿に許される主張／制約 |
| --- | --- | --- |
| YAML、Path、Intent | `src/yaml_config.c`、`samples/linux-vm-netns.yaml` | 対応schemaを読み込む。一般YAMLの完全対応ではない。入力例を現行のtraffic/path_selection/transition/fallback形式へ修正した。 |
| Explicit／Priority／Evaluated | `src/path_selection.c` の `en_select_path`、制約除外・比較関数 | 定義済み候補から選択する。任意トポロジからの経路探索ではない。 |
| 安定化 | 同ファイルの `exclusion_reason`、hysteresis判定 | hold-down、連続失敗／復旧閾値、hysteresisはC層実装済み。評価器の連続2回確認とは別であり、本番の全閾値検証ではない。 |
| Tunnel／Forwarding分離 | `include/eventnet/controller.h`、`src/transition.c` | 公開callbackはensure/remove tunnel、install/remove/active path、validate path。原稿のcreate/prepare/commit等は目標APIであり独立callbackの実装一覧ではない。 |
| 状態遷移 | `include/eventnet/types.h`、`transition_prepare`／`transition_commit`／`transition_confirm`／`rollback` | C enumと設計上のStable/Commit Applied等を区別した。active_path確認はcallback依存であり、E2E成功保証ではない。 |
| Graceful | `transition_commit` のpause/drain待機、評価器 `Runtime.trial` | C制御とmock試験、SA事前確認＋10ms待機の実通信反復。無損失・pause上限・Flow Preserveの完成ではない。 |
| Rollback | C `rollback`、評価器 `Runtime.routes`／`trial` | 片側のLinux route変更後に反対側を実適用失敗させ、両側Directへ復旧。全失敗点や本番eventnetd経路の実証ではない。 |
| TelemetryとAgent | `examples/eventnet_agent.c`、`src/telemetry.c`、`Runtime.observe` | 実ping観測・JSONL入力。認証付き遠隔操作、全拠点協調・並列測定、Agent単独Emergency Fallbackは実証していない。 |
| strongSwan | command adapter、任意libvici adapter、GRE smoke | 今回はswanctl経由のVICI＋Linux XFRM。libvici直接接続の今回の実測ではない。PSK実験であり証明書・長期rekey評価ではない。 |
| VPP | command adapter、生成plan、API transport | 今回はvppctl。Binary API transport境界と本番message codec／実FIB適用を区別する。 |
| GRE | [最終VM試験](evaluation/20261010-gre/README.md) | 双方向各3ping、ESP送受信、cleanup成功。1回の機能試験であり帯域・切替反復ではない。 |
| Native IPIP/IPsec | 原稿中の2026年9月CI実行番号 | 静的SA比較構成の過去CI結果。今回の8 CPU VM試験ではない。 |
| 実障害・通信品質 | [2 CPU](evaluation/20261010/README.md)、[8 CPU](evaluation/20261010-8cpu/README.md)、`Runtime.trial` | 各条件5回、各25件成功。Cで選択、Pythonでroute実適用する評価器。C Transition Engine全体の本番通信性能とはしない。 |
| VLAN／VRF／FIB | `Runtime.isolation`、同CSVの5件 | 手動CLIでIPv4の重複アドレスを別FIBへ分離。Controller全Intent、IPv6、全ACLの安全性ではない。 |
| 固定長管理 | `include/eventnet/types.h` | Node/Path各16、Tunnel32、Segment/Path8等の上限。大規模運用の性能保証ではない。 |
| RQ1／RQ2 | 共通計画・復旧の成立確認 | Backend交換行数・再利用率、単純route更新との対照比較は未実施。抽象化の初期的実現可能性と記録可能性まで。 |
| RQ3／RQ4 | Domain設計／Explainコードと回帰試験 | Federation未実装。Explain実装済みだが第三者の判断再構成評価は未実施。 |

## 数値照合

CSVのraw値から平均と標本SDを再計算し、2 vCPU表の28組、8 vCPU本文の10組、計38組のmean/SDを小数3桁で照合して一致した。両CSVは各25行すべてpass。error barは信頼区間ではなく標本SD。

CPU増設後は全core平均の飽和が解消したが、kernelも7.0.0-34から7.0.0-38へ更新されている。CPU増設だけの因果効果、単一coreの非飽和、測定中のVM全体RAM最大量までは示さない。daemon合計RSSとVM全体メモリ使用量は別指標。

## 修正した表現

- 9月25日の結果表へ10月の反復測定結果が混入していたため、当時未実施と明記して後節へ分離。
- API名、Capability、Health協調、Agent遠隔適用、状態名の目標設計と実装を明確化。
- 未実装とされていたhysteresis等を実装済みへ訂正。
- 簡易Gracefulの実通信反復は済、本番保証は未評価という表現へ統一。
- RQの有効性・安全性の優位比較は未完了と明記。
- GRE最終疎通の時点・環境・結果を追記。ログのSA鍵を除去して保存。

## 提出前の扱い

本稿は「Pathモデル・C制御ロジックと、評価器によるOSS実通信試験の初期評価」として記述できる。完成SD-WAN、本番eventnetdの自動切替、Flow Preserve、HA、FRR/BGP、Cloudの完成を結論に加えない。現在の原稿・測定器変更は未コミットであり、公開済み版と同一とは限らない。提出時にcommitと成果物を固定し、LuaLaTeXによるPDF確認を別途行う。
