# 8 vCPUでの反復実測（2026-10-10）

SSHでオンラインCPU 8個、メモリ8,272,678,912 bytes（約7.7 GiB、割当8 GB）を確認した。CPU構成は2 socket × 4 core × 1 thread。[旧2 vCPU測定](../20261010/README.md)は上書きしていない。

## 条件と結果

同じ計測器SHA256 `ddd33f022893faadb88e9173422abf8728e812a79a05b82e7fb691dd61d652be`、同じYAML、観測閾値2回、ICMP間隔50ms、trial 28秒、簡易Graceful待機10msで実行した。Immediate／簡易Gracefulの切替・復旧・片側失敗Rollback各5回、Direct／Hub帯域各5回、VLAN／FIB分離5回、計25ケースが成功した。切替試験10回すべてで同じTCP接続が完了した。

次表は平均±標本SD、各条件n=5。切替時間は検知からCommit完了までであり、障害発生からの通信断時間ではない。

| 指標 | Immediate | 簡易Graceful |
| --- | ---: | ---: |
| 検知時間（ms） | 337.134 ± 8.907 | 336.165 ± 6.868 |
| 切替時間（ms） | 172.607 ± 8.748 | 222.831 ± 91.317 |
| 最大応答間隔（ms） | 570.086 ± 37.188 | 620.205 ± 91.658 |
| ICMP損失（%） | 2.046 ± 0.115 | 2.208 ± 0.325 |
| VM全体CPU（%） | 14.499 ± 0.161 | 14.561 ± 0.555 |

無制限TCP（10秒）の受信帯域はDirect **157.889 ± 4.045 Mbit/s**、Hub **89.796 ± 6.987 Mbit/s**。VM全体CPUはそれぞれ15.861 ± 0.287%、13.226 ± 0.969%。切替試験のTCPは10 Mbit/sに制限しており、無制限帯域と混同しない。

## 比較と限界

前回の2 vCPUではImmediate切替1873.735ms、最大応答間隔3018.967ms、損失12.336%、無制限Direct帯域9.005 Mbit/s、VM全体CPU約99.8%だった。今回、全core平均CPUの飽和は解消し、これらの値は改善した。ただしCPU増設と同時にLinuxが **7.0.0-34 → 7.0.0-38-generic** に更新されたため、CPU増設だけの因果効果を分離した実験ではない。CPU平均の分母も2から8へ変わる。単一coreのボトルネックがないことまでは証明しない。

VPP 26.06-release、strongSwan 5.9.13は同じ。メモリ割当は変更なし。同一VMでの逐次・warm状態の反復であり、物理回線や独立したcold startの評価ではない。評価用route executorでの実測であり、常駐eventnetdの本番Adapter経路ではない。簡易Gracefulは既存SA確認＋10ms待機であり、Flow Preserveや無損失・通信断上限保証ではない。VLAN試験はIPv4の重複アドレスを持つtable 100/200の経路分離であり、全攻撃・全ACL条件を網羅しない。

## 成果物と再現

- [metrics.csv](metrics.csv): 25ケースの値。
- [statistics.csv](statistics.csv): n、平均、中央値、標本SD、min/max、nearest-rank P95。
- [environment.json](environment.json): CPU、メモリ、kernel、設定、ソース・データSHA256。
- [切替時間](figures/transition_ms.png)、[最大応答間隔](figures/max_reply_gap_ms.png)、[帯域](figures/throughput_mbps.png): CSVから生成した平均±標本SD。PDFも保持する。
- 生ログ・JSONLは `out/paper-live-20261010-8cpu`。終了後にsite-a/site-b/hub-1のプロセスが残っていないこと、試験用PSKの削除、元のVPP service復帰を確認した。

リポジトリrootから、専用VMで新しい出力先を指定する。

```sh
sudo REPEAT=5 OUT_DIR=out/paper-live-8cpu-new sh scripts/vm-paper-collect-metrics.sh samples/linux-vm-netns.yaml --isolation
```

共有フォルダが再起動後に未mountだったため、今回の測定前に再接続した。準備失敗は測定標本に含めていない。
