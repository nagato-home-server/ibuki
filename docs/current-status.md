# 現在の実装と検証状況

更新日: 2026-10-10。機能疎通の対象コードは `0e27496`。追加計測は `806bc6b` に未コミットの測定器変更を加えた状態で行い、測定器SHA256とYAML SHA256を保存した。過去の日誌・CI結果は、その時点の証拠として区別する。

## 2026年10月10日の反復実測

CPU増設後の8 vCPU・メモリ8 GB条件でも各5回、計25ケースが成功した。Immediateの平均検知337.134ms、検知後Commit172.607ms、最大応答間隔570.086ms、損失2.046%。Direct／Hub帯域は157.889／89.796 Mbit/s。全core平均CPUは約13〜16%で、前回の飽和は解消した。[8 vCPU測定結果](evaluation/20261010-8cpu/README.md)を参照する。カーネルも更新されたためCPU増設だけの効果とは断定しない。以下の数値は保存した旧2 vCPU条件である。

実リンク障害・復旧・片側失敗rollbackをImmediate／簡易Gracefulで各5回、Direct／Hub TCP帯域を各5回、重複アドレスのVLAN／FIB分離を5回、合計25ケース成功した。TCPは切替10試験で同じ接続を完了した。平均検知は約1.04〜1.09秒、検知後Commitまで約1.87〜2.05秒、最大応答間隔は約3.02〜3.16秒だった。

これは事前確立SAと評価用route executorによる実測であり、常駐eventnetdの本番Adapter実行、無損失Graceful、pause上限保証の完成を意味しない。VM CPUは約99.8%であり、約9 Mbit/sの帯域をVPP一般性能として引用しない。VLAN分離は手動CLIによるIPv4 FIB分離の確認。

数値、CSV、グラフ、条件と制約は [旧2 vCPU測定結果](evaluation/20261010/README.md)、関数と実行方法は [計測手順](live-measurement-protocol.md) を参照する。以下の現状表は10月10日の確認を反映する。実験用計測の追加と本番実装の完了は区別する。

## 実装と確認範囲

8 vCPU条件でGRE最終疎通も双方向各3ping・ESP送受信・cleanupまで成功した。[GRE証拠](evaluation/20261010-gre/README.md)と[原稿・コード・測定の照合表](paper-claim-audit-20261010.md)を参照する。原稿の設計API、Agent構想、本番制御保証を実装済みの実測と混同しないよう修正した。

| 項目 | 現在の状態 | 残る確認・実装 |
| --- | --- | --- |
| Explicit／Priority／Evaluated | Cの選択ロジック、制約、除外理由、シナリオを実装 | 多拠点・長時間条件での誤切替と収束 |
| Telemetry | Agent逐次測定、Cのyyjson JSONL入出力、鮮度検証、Controller入力。評価器でAgent検知から切替まで計測 | Agent並列測定、本番eventnetd実Adapterの連続運用、負荷・時刻整合性 |
| Immediate／簡易Graceful | C制御・mock試験。評価器で各5回の実切替とTCP継続を両CPU条件で確認 | C Transition Engine全工程の実運用、flow追跡、pause上限保証 |
| Rollback | C executor／生成計画と制御試験。評価器の片側route失敗から復元を各戦略5回確認 | 全工程の失敗注入、旧SA撤去後の復旧、実Adapterの障害復旧 |
| Direct／Hub統合 | Controller生成計画、VPP転送、XFRM暗号化、双方向client疎通。実リンク切断・復旧を反復測定 | 複数VM・物理WAN評価、eventnetd本番ループの収束 |
| GRE over IPsec | VPP GRE＋strongSwan/XFRM、双方向client疎通、ESP進行、終了後cleanupをVMで確認 | MTU、帯域、長時間運用、再鍵交換中の通信 |
| VPP Native IPIP/IPsec | 過去のCIで静的SAによる疎通・再適用を確認 | 今回のVM修正後の再検証、IKE・SA同期・鍵更新。GREとは別方式 |
| VLAN／VRF／FIB | YAML、計画生成、CLI adapter。手動CLIのVLAN 100/200・重複IPv4・FIB分離を各CPU条件5回確認 | Controller Intent全体の実適用、未タグ／一覧外VLAN遮断、IPv6、物理trunk |
| VICI／Binary API | swanctl経由の実VICI操作、任意libvici接続・監視境界、VPP API transport境界を実装 | VPP版依存route／VLAN／VRF codecと本番運用 |
| 規模 | 固定長配列と上限検証によるインメモリモデル | 上限内の負荷試験、必要に応じたモデル拡張 |
| FRR、HA、Flow Preserve、GUI | 今回の実疎通確認対象ではなく、後段の拡張 | 未踏期間の優先順位に沿って実装 |

## 現行データパス

2026-10-10のZ3による追加デバッグで、Evaluatedのhysteresisが除外済みActive Pathを再選択する不具合を確認し修正した。閾値モデルから生成した反例を実Cへ投入し、制約違反・無効化・候補外・waypoint不足と正常境界の10ケース、およびWSL CTest29件を確認した。これは経路選択の追加回帰検証であり、既存VM実測を修正版で再取得したものではない。C全体や固定長配列のメモリ安全性の形式証明ではない。

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

この10月9日のFallbackは `--active-path path-direct --fail-path path-direct` という障害イベント入力で確認した。当時未計測だった実リンク切断とAgent検知から切替までの時間は、10月10日に評価器で計測した。

証拠は [日誌](../daily/20261009.md) とローカル生成ログ `out/revalidation-20261009/integrated-confirmed.log`、`hub-sas.log`、`gre.log`。ログは生成物であり、クローンだけでは取得できない。XFRM表示にはSA鍵が含まれ得るため、生ログを公開する前に秘密情報を除去する。

VMのCビルド・unit testと、修正後のplan generatorによる実runtime適用を確認済み。最新コミットのWindows／CMake／GitHub CI成功をこの結果から推定しない。過去のCTest 27件成功は2026-09-25の結果である。

## 論文提出までの残作業

実障害・復旧・部分Rollback、ICMP/TCP、帯域、CPU/RSS、手動VLAN/FIB分離の反復CSV・統計・図を保存し、原稿の38組の平均・標準偏差と照合済み。WSLのCTestは29件、Python計測器テストは26件を確認した。過去の27件と区別する。

残作業は提出revision/build結果の固定とPDF組版確認である。具体的な確認項目は本書の「提出成果物の固定手順」に集約する。最大応答間隔は厳密な最大通信断ではなく、全core平均CPUとdaemon RSSでは単coreやVM RAM peakの飽和を判定できない。

UDP、MTU掃引、Evaluated実転送反復、GRE帯域反復、全工程Rollback、長時間運用は追加評価である。今回未測定の性能を主張しない限り、現在の論文範囲の完了条件へ後から追加しない。

## 未踏期間の範囲

多拠点・物理機・クラウドでの閾値調整とGraceful評価、VPP Binary API codec、証明書・鍵のライフサイクル、FRR／BGPと経路収束、HAと再起動復旧を優先する。Flow Preserveは既存flow識別と保持が成立する見通しを確認して着手する。GUI、履歴DB、クラウド固有APIは後段とする。閾値はユーザ設定可能なまま維持し、推奨値を環境別の実験結果として示す。

詳細は [期間別ロードマップ](paper-to-mitou-roadmap.md)、[実装計画](paper-to-mitou-implementation-plan.md) を参照する。

## 論文評価の再現手順

専用VMのリポジトリルートで実行する。namespace/route/SAを変更するため他の通信試験と並行実行しない。

```sh
sh scripts/vm-build-cc.sh
sudo REPEAT=5 OUT_DIR=out/paper-live-recheck \
  sh scripts/vm-paper-collect-metrics.sh samples/linux-vm-netns.yaml --isolation
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

CSV、環境情報、caseログ、統計とSVGを自動生成する。テンプレートCSVへの手入力を実測として扱わない。[実測プロトコル](live-measurement-protocol.md)を参照。

Nativeの別途再検証は次の入口を使う。反復CSVへ自動追加されるケースではない。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-vpp-native.yaml
```

## 図と統計

```sh
python3 scripts/generate-paper-graphs.py \
  --metrics-csv docs/evaluation/20261010-8cpu/metrics.csv \
  --out-dir out/paper-figures --publication
```

SVG・集計CSVはPython標準ライブラリで生成できる。`--publication`のPNG/PDFはmatplotlibが必要で、測定wrapperではなく図生成プログラムへ渡す。

n=5の平均と標本標準偏差。nearest-rank P95はこの標本数では最大値。検知、検知後Commit、応答間隔、損失、再送、帯域、CPU、RSSを区別する。最大応答間隔は厳密な最大通信断ではない。CLI時間やsmoke全体時間を実切替時間に置き換えない。

## 提出成果物の固定手順

1. 提出revisionと差分、YAML、測定器/binaryのSHA256を固定し、測定時の未コミット状態を明記する。
2. 提出版のbuild/CI結果を保存する。旧CI成功は現在の作業ツリーの保証ではない。
3. `研究内容.tex`をPDF化し、図表、参照、フォント、改ページ、本文を確認する。
4. 指標・未検証範囲を文書間で一致させ、公開ログから秘密情報を除去する。

実障害・反復・グラフ取得は完了済み。UDP、MTU掃引、Evaluated実転送反復、GRE帯域、全工程Rollback、長時間運用は追加評価であり、その性能は主張しない。
