# 論文提出前の最低限成果物

論文提出時点では、未実装の大規模機能を増やすより、実装済みの制御層を再現可能な評価として固める。以下を提出前の完了条件とする。

## 実装

- C実装をLinux・Windowsでビルドできる。
- CTestでPath Selection、YAML route、Agent telemetry、threshold、状態遷移、reloadを再現できる。
- Direct IPsecでstrongSwanのIKE／CHILD_SA確立、LAN間疎通、ESP counter増加を確認できる。
- Hub fallbackでDirect障害からHub経路へ切り替わり、LAN間疎通を確認できる。
- VPPは生成されたroute／VLAN／VRF planと、利用可能なVMでの実forwardingを別々に記録する。
- strongSwan/XFRM backendのVPP GRE over IPsecは、namespace v2で双方向LAN疎通、ESP送受信counter増加、停止後の残留なし、再適用を確認済みとして記録する。
- VPP Native backendは、GREではなくIPIP + `ipsec tunnel protect`の実験構成である。双方向疎通とVPPの暗号化・復号counter増加は確認済みだが、静的SA・静的鍵を使うため本番Backend完成とは扱わない。
- 過去のnamespace制約は`docs/gre-namespace-constraint.md`を履歴として参照し、現在の構成と実行手順は`docs/namespace-runtime-v2.md`を正とする。

## 評価データ

最低限、次の同一条件の反復データを保存する。

- Direct正常系
- Direct障害からHubへのFallback
- Direct復旧後の再選択
- Priority選択とEvaluated選択
- Immediateと、実装できた場合のGraceful

実測CSVはroot権限を持つLinux VMで次のように収集する。各ケースのログはCSVと同じ場所に残る。

```sh
REPEAT=5 OUT_FILE=out/paper-metrics.csv \
  sudo -E sh scripts/vm-paper-collect-metrics.sh samples/linux-vm-netns.yaml
```

GREの実データパスは、VPPとstrongSwanが導入済みのVMでnamespace v2の入口から実行する。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

VPP Native IPsec/IPIPとの比較は、同じ入口へnativeサンプルを渡して実行する。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-native.yaml
```

各反復では、transition時間、packet loss、最大通信断時間、RTT、TCP retransmission、CPU使用率、メモリ使用量を記録する。収集Scriptが自動取得できない項目は、同時に保存したログと`/usr/bin/time`、`pidstat`等で補完する。未実装のFlow Preserveは測定対象に含めず、設計上の将来課題として扱う。

測定値は`samples/paper-metrics-template.csv`をコピーして記入する。空欄のまま提出用グラフを作成してはならない。

## 図の生成

`vm-evaluate.sh`の出力と実測CSVから、Python標準ライブラリだけでSVGを生成する。

```sh
python3 scripts/generate-paper-graphs.py \
  --summary-csv out/evaluation/YYYYMMDD-HHMMSS/summary.csv \
  --metrics-csv out/paper-metrics.csv \
  --out-dir out/paper-figures
```

生成物:

- `evaluation-status.svg`: pass／partial／skip／failの件数
- `evaluation-duration.svg`: 評価caseごとの処理時間
- `transition-time.svg`: scenarioごとの平均切替時間
- `packet-loss.svg`: scenarioごとの平均packet loss
- `metrics-summary.csv`: 平均値と標準偏差

SVGはTeXへ取り込む前に、評価時のcommit、YAML、OS、kernel、VPP／strongSwanのバージョンと対応付ける。既存の評価ログを後から編集せず、再実行で得たCSVを採用する。

## 論文へ書く範囲

論文では、Tunnel Resource抽象化、Path Selection、Explain、Failure／Fallback、strongSwan／VPPへのplan接合を実装済みとして示す。一方、Flow Preserve、VPP Binary APIの版依存codec、FRR／BGP、HA、Federation、Cloud VPN Adapter、GUIは未実装または未検証の拡張として明記する。

実装の成功と実環境の成功を混同しない。plan生成がpassでも実データパスが未確認ならpartialまたは未検証とし、runtimeの疎通結果、ログ、生成planを同じ評価ディレクトリへ保存する。

## 2026-09-25時点の進捗と提出までの残作業

完了済みの機能確認は、root不要のCTest 27件、strongSwan/XFRMによるGRE over IPsec、VPP Native IPsec/IPIPの双方向暗号化通信である。後者二つはGitHub Actions実行
[36022032962](https://github.com/nagato-home-server/ibuki/actions/runs/36022032962)と
[36022029474](https://github.com/nagato-home-server/ibuki/actions/runs/36022029474)で、再適用と停止後残留確認を含めて成功した。

提出を止める残作業は次のとおりである。

1. Direct正常、Hub fallback、Direct復旧、Priority/Evaluated、Immediate/Gracefulを同一条件で5回以上測定し、正式な`out/paper-metrics.csv`を作る。
2. 現在の収集scriptが記録するsmoke全体時間を、Decision、Prepare、Validate、Commit、Post Validation、Rollbackへ分解する。最大通信断、RTT、reordering、TCP retransmission、CPU、メモリの空欄も実測で埋める。
3. 提出対象commitでroot不要validationとroot必須runtimeをまとめて再実行し、環境manifest、summary、各ログ、生成planを一つの成果物として保存する。Windows buildも同じcommitで再確認する。
4. `generate-paper-graphs.py`で図と統計CSVを生成し、`研究内容.tex`の評価表・考察を正式データへ置き換える。
5. TeXをPDF化して、図表、参照、改ページ、フォント、主張と証拠の対応を最終確認する。

本番化の残作業であり論文提出の必須条件にはしないものは、VPP NativeのIKE/鍵更新とSA同期、VPP Binary APIの版依存codec、strongSwanのrekey/DPD運用、FRR/BGP/OSPF、HA、Flow Preserve、実trunk分離、GUIである。
