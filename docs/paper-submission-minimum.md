# 論文提出前の最低限成果物

論文提出時点では、未実装の大規模機能を増やすより、実装済みの制御層を再現可能な評価として固める。以下を提出前の完了条件とする。

## 実装

- C実装をLinux・Windowsでビルドできる。
- CTestでPath Selection、YAML route、Agent telemetry、threshold、状態遷移、reloadを再現できる。
- Direct IPsecでstrongSwanのIKE／CHILD_SA確立、LAN間疎通、ESP counter増加を確認できる。
- Hub fallbackでDirect障害からHub経路へ切り替わり、LAN間疎通を確認できる。
- VPPは生成されたroute／VLAN／VRF planと、利用可能なVMでの実forwardingを別々に記録する。
- GRE over IPsecは計画生成とstrongSwan SA確立を確認し、VPP実データパスが未確認なら未検証として記載する。
- GREのnamespace制約と未検証範囲は`docs/gre-namespace-constraint.md`に従って記載する。

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
