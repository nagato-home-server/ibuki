# 現在の実装と検証状況

更新日: 2026-10-09。対象コードは `0e27496`（VPP名前空間の実疎通とIPsec・GRE統合を修正）。過去の日誌・CI結果は、その時点の証拠として区別する。

## 実装と確認範囲

| 項目 | 現在の状態 | 残る確認・実装 |
| --- | --- | --- |
| Explicit／Priority／Evaluated | Cの選択ロジック、制約、除外理由、シナリオを実装 | 多拠点・長時間条件での誤切替と収束 |
| Telemetry | Agent逐次測定、yyjson JSONL入出力、鮮度検証、Controller入力を実装 | Agent並列測定、実測値から自動切替までの連続評価、負荷・時刻整合性 |
| Immediate／簡易Graceful | 制御ロジックとmock試験を実装 | 実通信中の戦略比較、TCP継続性、損失・順序変化 |
| Rollback | executor／生成計画と制御試験を実装 | 実runtimeの工程別失敗注入とStable Pathへの復旧評価 |
| Direct／Hub統合 | Controller生成計画、VPP転送、strongSwan/XFRM暗号化、双方向client疎通をVMで確認 | 実リンク障害の検知・切替時間、復帰時の通信影響 |
| GRE over IPsec | VPP GRE＋strongSwan/XFRM、双方向client疎通、ESP進行、終了後cleanupをVMで確認 | MTU、帯域、長時間運用、再鍵交換中の通信 |
| VPP Native IPIP/IPsec | 過去のCIで静的SAによる疎通・再適用を確認 | 今回のVM修正後の再検証、IKE・SA同期・鍵更新。GREとは別方式 |
| VLAN／VRF／FIB | YAML、計画生成、CLI adapterと試験入口を実装 | 現行namespace構成でタグ付き実パケット・trunk・分離を再検証 |
| VICI／Binary API | swanctl経由の実VICI操作、任意libvici接続・監視境界、VPP API transport境界を実装 | VPP版依存route／VLAN／VRF codecと本番運用 |
| 規模 | 固定長配列と上限検証によるインメモリモデル | 上限内の負荷試験、必要に応じたモデル拡張 |
| FRR、HA、Flow Preserve、GUI | 今回の実疎通確認対象ではなく、後段の拡張 | 未踏期間の優先順位に沿って実装 |

## 現行データパス

LAN端末は独立した `client-a`／`client-b` namespace。VPP LAN gatewayは `10.10.1.1`／`10.10.2.1`、clientは `10.10.1.2`／`10.10.2.2`。VPPとcharonはsite namespaceで同居し、node別CLI socketへ計画を適用する。

- VPP単体Direct／Hub: 専用VPP transit vethを使用する。
- IPsec統合Direct／Hub: endpointのVPPからTAPでLinuxへ渡し、XFRMと既存underlayで暗号化転送する。`VPP_TOPOLOGY_MODE=ipsec` は平文VPP transitを作らない。Hubの暗号化中継はLinux/XFRMが担当する。
- GRE: VPPがGREを生成し、Linux peerを通じてstrongSwan/XFRMがGRE outer通信を保護する。試験は `SKIP_GRE=1 SKIP_IPSEC_TAP=1` で補助構成を作り、GREの所有者をController生成計画に限定する。

GRETAP、VXLAN、L2 bridgeによるL2延伸は採用していない。

## 2026-10-09の機能疎通測定

環境はUbuntu VM、Linux 7.0.0-34、VPP 26.06、2 vCPU。VPP host-interfaceに `cksum-gso-disable` を指定し、試験用の `VPP_NS_POLL_SLEEP_USEC=1000` を使用した。これらは測定条件であり、性能最適値ではない。

| 構成 | 往路平均RTT | 復路平均RTT | 測定ping |
| --- | --- | --- | --- |
| Controller Direct統合 | 29.130 ms | 37.600 ms | 各3/3、損失0% |
| Controller Hub統合 | 18.051 ms | 20.172 ms | 各3/3、損失0% |
| GRE＋strongSwan/XFRM | 22.950 ms | 16.649 ms | 各3/3、損失0% |

測定前に各方向1回のARP warm-upを行い、その出力も保存した。初回往路のwarm-upはDirect、Hub、GREでそれぞれ1パケット損失した。上表はwarm-up後の機能確認であり、起動直後の無損失や性能優位を示すものではない。3標本の平均を使ってDirectとHubの性能を比較しない。

Fallbackは `--active-path path-direct --fail-path path-direct` という障害イベント入力で選択・適用を確認した。連続通信中の実リンク切断、Agent検知から切替までの時間は未計測。

証拠は [日誌](../daily/20261009.md) とローカル生成ログ `out/revalidation-20261009/integrated-confirmed.log`、`hub-sas.log`、`gre.log`。ログは生成物であり、クローンだけでは取得できない。XFRM表示にはSA鍵が含まれ得るため、生ログを公開する前に秘密情報を除去する。

VMのCビルド・unit testと、修正後のplan generatorによる実runtime適用を確認済み。最新コミットのWindows／CMake／GitHub CI成功をこの結果から推定しない。過去のCTest 27件成功は2026-09-25の結果である。

## 論文提出までの残作業

1. 連続ICMP／TCP／UDP通信中に障害を注入し、検知時間、切替工程別時間、最大通信断、損失、再送を取得する。
2. Direct、Fallback、Recovery、Priority／Evaluated、Immediate／簡易Gracefulを同一条件で5回以上反復する。
3. Rollback失敗注入、VLAN／VRF実パケット分離、MTU影響を検証する。
4. 帯域、CPU、メモリを取得し、commit、YAML、OS、依存版、オフロード、poll設定をmanifestへ保存する。
5. CSV・グラフと本文を対応させる。smoke全体の実行時間を通信経路の切替時間として使わない。

## 未踏期間の範囲

多拠点・物理機・クラウドでの閾値調整とGraceful評価、VPP Binary API codec、証明書・鍵のライフサイクル、FRR／BGPと経路収束、HAと再起動復旧を優先する。Flow Preserveは既存flow識別と保持が成立する見通しを確認して着手する。GUI、履歴DB、クラウド固有APIは後段とする。閾値はユーザ設定可能なまま維持し、推奨値を環境別の実験結果として示す。

詳細は [期間別ロードマップ](paper-to-mitou-roadmap.md)、[実装計画](paper-to-mitou-implementation-plan.md)、[論文提出条件](paper-submission-minimum.md) を参照する。
