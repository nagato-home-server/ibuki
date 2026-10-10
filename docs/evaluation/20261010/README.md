# 2026年10月10日の実リンク障害と通信品質の評価結果

Ubuntu VM、2 vCPU、Linux 7.0.0-34、VPP 26.06、strongSwan 5.9.13で、5種類を各5回、合計25ケース実行し、すべて成功した。各切替trialでは実リンクdown、Hubへの切替、リンクup後のDirect復帰、片側Commit失敗後のrollbackを行った。単一TCP接続は切替10試験すべてで完了した。

測定器は評価専用Python executorであり、C ControllerのPath判断を利用するが、eventnetd本番ループ全体の評価ではない。Immediateと簡易Gracefulは、事前確立済みSAを使用し、後者に検証後10msのCommit前待機を加えたもの。Intentの `max_pause_ms` に基づく本番遷移の厳密なpause上限を測定・保証した結果ではない。

## 指標と結果

表は平均 ± 標本標準偏差、各n=5。時間はms、損失は全28秒の往路ICMP送信に対する割合。最大応答間隔は各trialの最大値を集計したものであり、厳密な無通信時間ではない。

| 指標 | Immediate | 簡易Graceful |
| --- | ---: | ---: |
| Agent検知 | 1090.518 ± 190.292 | 1037.881 ± 208.700 |
| C判断・プロセス起動込み | 195.652 ± 61.713 | 194.531 ± 41.889 |
| SA確認・Prepare | 1123.704 ± 205.288 | 1318.140 ± 413.649 |
| 両側route Commit | 505.573 ± 121.244 | 465.409 ± 93.273 |
| 検知成立からCommit完了 | 1873.735 ± 256.916 | 2047.123 ± 537.846 |
| 障害操作開始から疎通検証完了 | 3701.142 ± 520.947 | 3733.122 ± 640.364 |
| 最大応答間隔 | 3018.967 ± 399.245 | 3156.855 ± 478.429 |
| ICMP損失率 | 12.336 ± 0.534% | 13.259 ± 3.403% |
| TCP再送数・trial全体 | 67.600 ± 42.117 | 49.600 ± 31.997 |
| Direct復帰検知 | 1239.083 ± 218.763 | 1330.734 ± 594.819 |
| Direct復帰Commit | 652.261 ± 173.470 | 1119.467 ± 633.209 |
| 部分失敗検出後のrollback | 479.898 ± 118.849 | 751.532 ± 522.287 |
| VPP／charon合計CPU | 49.919 ± 0.900% | 46.671 ± 6.227% |
| VPP／charon合計RSS | 460.335 ± 0.204 MiB | 460.398 ± 0.233 MiB |

10M上限での切替中TCP受信帯域はImmediate 4.591 ± 0.435 Mbit/s、簡易Graceful 4.385 ± 1.207 Mbit/s。別枠の無制限TCP、10秒×各5回ではDirect 9.005 ± 0.673 Mbit/s、Hub 9.317 ± 1.267 Mbit/sだった。後者をVPPやIPsecの一般的な最大性能とは扱わない。

## 分離と終了後の確認

VLAN100／200を別VPP FIB tableへ手動CLI設定し、同じIPアドレスを持つclient同士で両方向疎通を確認した。FIB100のremote LAN routeだけを削除すると100は全損失、200は疎通を維持した。100のroute復元後は疎通を回復した。この一連の確認は5/5成功した。Controllerが任意のVLAN Intentを自動適用する全経路、L2延伸、全攻撃パターン、IPv6、ACL fail-closedを検証した結果ではない。

測定終了後、site-a／site-b／hub-1に残留プロセスがないことをSSHで確認し、一時停止していたシステムVPPを戻した。試験用設定のPSKを公開データに含めていない。

## 結果の読み方と制約

VM全体CPUは切替試験で約99.8%、帯域試験で約99.7%となり、CPU飽和したソフトウェアtestbedの値である。daemon CPUは1 core=100%で、2 vCPU合計では200%まで取り得る。一方、VM全体CPUは全coreを平均した0〜100%の指標。daemon合計にはLinux XFRMのkernel／softirq処理や測定プロセスを含めない。合計RSSは共有ページの重複計上を含む。

TCP再送はtrial全期間の合計であり、個々の切替工程に厳密に帰属させていない。無制限TCPにはslow startも含める。ICMP送信間隔50ms、終了後の返信待ち1秒、Agent連続観測2回という設定であり、すべての故障形態やflowに対する通信断上限ではない。

Gracefulの損失低減やHubの性能優位は示していない。n=5のばらつきとCPU飽和を考慮すると、そのような結論は出せない。実eventnetd Adapter経路、実回線、多vCPU・CPU affinity、MTU／MSS、kernel暗号処理負荷、同一負荷での無障害controlを追加評価する必要がある。

## データと再現

- [metrics.csv](metrics.csv): 25行の測定値。逆方向、phase別損失、連続損失、RSS／CPU最大値も含む。
- [statistics.csv](statistics.csv): 指標ごとのn、平均、中央値、標本SD、min、max、nearest-rank P95。
- [environment.json](environment.json): 基準commit、測定器SHA256、YAML SHA256、条件、公開CSV SHA256。未コミットの測定器変更を含むことを明示。
- [最大応答間隔](figures/max_reply_gap_ms.png)、[損失](figures/packet_loss.png)、[帯域](figures/throughput_mbps.png)、[CPU](figures/cpu_percent.png): 平均±標本SDのグラフ。各主要図のPDFも同じフォルダにある。
- [再現手順と関数責務](../../live-measurement-protocol.md)。raw JSONL、command log、全SVGはローカルの `out/paper-live-20261010-verified` に保持。

過去の予備試験や出力先修正前に中止した `out/paper-live-20261010-final` の値は、この公開CSVに含めていない。グラフはCSVから再生成可能であり、標本SDのerror barは信頼区間ではない。
