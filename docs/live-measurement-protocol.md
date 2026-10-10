# 実リンク障害と通信品質の測定手順

この測定は、専用Linux VM上でclient通信を継続しながら、Direct underlayのリンクを実際に停止する。実測Agentによる障害検知、C Controllerによる候補選択、実XFRM経路の適用を個別に記録する。以前の統合runtime全体の起動時間を `transition_ms` と呼ぶ集計は廃止した。

## 実行と前提

リポジトリのルートで実行する。既存の `site-a`、`site-b`、`hub-1`、`relay-c`、`client-a`、`client-b` は試験専用であること。実行中はこれらのnamespaceを再作成し、システムVPPが稼働している場合は一時停止して終了後に戻す。別のruntime試験を同時実行しない。

```sh
sudo apt-get install -y iperf3 python3
sh scripts/vm-build-cc.sh
sudo REPEAT=5 sh scripts/vm-paper-collect-metrics.sh samples/linux-vm-netns.yaml --isolation
```

出力先は `out/paper-live-YYYYMMDD-HHMMSS`。`OUT_DIR`、`BUILD_DIR`、`REPEAT` を環境変数で変更できる。`OUT_FILE` を指定すると最終CSVをその名前にもコピーする。Python引数はYAML引数の後に渡す。

| 引数 | 初期値 | 意味 |
| --- | --- | --- |
| `--duration` | 28秒 | 1切替試験のICMP送信期間、25〜600秒 |
| `--threshold` | 2 | 同じ健康状態の連続観測数 |
| `--probe-interval` | 0.25秒 | Agent呼び出し間の待機。ping実行時間を含む実周期ではない |
| `--ping-interval` | 0.05秒 | 双方向ICMP送信間隔 |
| `--drain-ms` | 10ms | 簡易Graceful試験のCommit前待機 |
| `--tcp-rate` | 10M | 切替中の単一TCP負荷上限。最大帯域とは異なる |
| `--isolation` | 無効 | VLAN100／200の実パケット分離試験を追加 |

## 試験構成と実装上の境界

LAN側は `client-a → site-a VPP → Linux XFRM → site-b VPP → client-b`。Hub時はXFRMの中継に `hub-1` を加える。DirectとHubのCHILD SAを事前確立し、VPPのLAN接続は維持したまま、Linuxのremote LAN routeを切り替える。Directは試験専用のXFRM if_id=103を使う。

経路判断には `eventnet_scenario` のC実装を使用する。Agentは実underlay endpointへのpingを測定し、Pythonが連続観測閾値を判定してC Controllerに障害イベントを入力する。**常駐eventnetdがTelemetry受信から実Adapterを経由して自動Commitする本番ループの測定ではない。** Pythonの実経路executorを使うため、測定結果をeventnetd本番遷移性能と呼ばない。

Immediate／簡易Gracefulは事前確立構成で比較する。後者はStandby SAを確認後、指定時間を待ってCommitする試験用最小ポリシーである。旧SAの動的解放、flow単位保持、完全なmake-before-break制御、通信断ゼロを証明するものではない。実障害時は旧リンクが既に使用不能なので、待機が損失を減らすとは限らない。

## 測定順序

1. ARP warm-up後、両方向の基本通信を確認する。
2. 双方向ICMPと、再接続しない単一iperf3 TCPセッションを開始する。
3. `a-direct` をdownにし、実Agentの連続Failedを待つ。
4. C ControllerのFallback判断、Standby SA確認、両拠点route Commit、通信再確認を行う。
5. リンクをupにし、Agentの連続Healthyを待ち、Directへ復帰する。
6. 片側のHub routeだけ成功させ、反対側に存在しないinterfaceを指定して実Commitを失敗させる。例外後に両側をDirectへ戻す。
7. 切替試験とは別にDirect／Hubそれぞれの無制限TCPを10秒測定する。
8. VLAN試験を指定した場合、重複IPアドレスを持つVLAN100／200を別FIB tableに接続する。両方向疎通後、table100の経路だけ削除して100が遮断され、200は通信を維持すること、100を復元すると再疎通することを確認する。

VLAN／VRF試験はL3 forwarding table分離の確認であり、全方向のACL、任意のVLAN攻撃、IPsec fail-closedの証明とは異なる。

## 時刻と指標の定義

全工程とraw ICMP記録は同一VMのmonotonic clockを使用する。別機器間の時計同期に依存しない。

ICMPは指定時間だけ送信し、その後1秒間返信を待って終了する。締切より後の返信は未受信として扱う。phase別値はその測定窓内に送信・受信されたものから算出する。送信間隔はschedulerの影響で設定値より長くなる場合があるため、実送信時刻もJSONLに残す。

- `detection_ms`: リンクdown操作完了からAgent閾値成立まで。
- `decision_ms`: C Controllerプロセス起動から判断完了まで。プロセス起動費用を含む。
- `prepare_ms`: 判断完了から事前確立SAの再確認完了まで。SA新規確立時間ではない。
- `commit_ms`: 両端のremote LAN routeの適用開始から完了まで。
- `post_validation_ms`: Commit完了から追加pingの成功まで。
- `transition_ms`: Agent検知成立からCommit完了まで。障害検知時間は含めない。
- `fault_to_validated_ms`: リンクdown操作開始から切替後の疎通検証完了まで。
- `max_reply_gap_ms`: 連続成功応答間の最大間隔。測定窓両端の未応答期間も含める。
- `outage_estimate_ms`: 最大応答間隔から設定送信間隔を引いた参考値。RTT・scheduler jitterを含み、真の無通信時間の厳密値ではない。
- `max_loss_burst`: 連続して応答が観測できなかった送信数。
- `packet_loss`: 全測定期間に送ったICMPの損失率。phase別lossと逆方向値も保存する。
- `packet_reordering`: ICMP返信sequenceの順序逆転数。全データパケットの順序検証ではない。
- `tcp_session_completed`: 同じTCPクライアントプロセスが試験終了まで完了したこと。再送数はiperf3の送信側値。
- `throughput_mbps`: receiver側の実受信帯域。切替caseは10M制限、別caseは無制限TCP。
- `cpu_percent`: namespace内VPP／charon合計のCPU使用率。1 core=100%、2 vCPUでは200%、8 vCPUでは800%まで。VM全core平均の0〜100%とは尺度が異なる。
- `memory_mb`: 同じdaemonの合計RSS、単位MiB。共有ページの重複計上がありPSSではない。
- `host_cpu_percent`: `/proc/stat` のidle/iowait差分から計算するVM全体の参考負荷。

CPU／RSSは250ms間隔。平均CPUは実サンプリング時間で加重する。Python測定器、iperf3、Agent、C判断プロセスのCPUはdaemon合計に含めない。CPU pinningや実HW回線の条件を固定した評価ではないので、2 vCPUでの値を製品性能や方式優位の根拠にしない。

試験は同じVPP／SAを維持して逐次反復する。毎回のcold startや独立した機器・回線の反復ではなく、温まった同一testbedのばらつきを記録する。daemon合計CPUにはLinux XFRMのkernel暗号化／softirq負荷を含めないため、VM全体CPUも別指標として保存する。

## 出力と再集計

`manifest.json` にGit revision、未コミット状態、YAML SHA256、OS／VPP／strongSwan、CPU数、引数を保存する。`commands.log`、`events.jsonl`、`agent.jsonl` と各trialの双方向ICMP JSONL、TCP JSON、resource JSONLを保存する。PSK入りの設定は許可された `/etc/swanctl` 配下で読み込み、cleanup時に削除し、成果物へコピーしない。

`metrics.csv` は完了済みcaseだけを逐次保存する。途中失敗は `failure.json` とログに記録し、成功caseや0損失へ変換しない。既存manifestがある出力先への上書きは禁止する。評価器はlockで同時実行を禁止する。

```sh
python3 scripts/generate-paper-graphs.py --metrics-csv out/計測結果/metrics.csv --out-dir out/計測結果/figures
python3 -m unittest discover -s tests -p test_paper_runtime.py -v
```

集計はscenarioとstrategyを分離し、指標ごとにn、平均、中央値、標本標準偏差、最小、最大、nearest-rank P95を保存する。SVGは平均と標本SDのerror barを表示する。欠測、失敗、NaN、無限大は集計から除外し0へ置換しない。n=5のP95は最大値に一致し、母集団の95パーセンタイルの精密推定ではない。

Matplotlibがある環境ではグラフ生成コマンドに `--publication` を追加すると、主要8指標のPNGとPDFも生成する。このオプションは `generate-paper-graphs.py` 専用であり、runtime測定の引数ではない。標準SVGと統計CSVの生成にMatplotlibは不要。

## 関数と責務

`paper-runtime.py` の `probe` はraw ICMPを記録し、`checksum` はパケットのchecksumを生成する。`ping_metrics` はsequenceで照合し、重複応答を二重計上せず損失・gap・逆転を集計する。`Resources.collect/finish` はdaemon資源の生データと加重平均を生成する。

`Runtime.setup` は既存試験を片付けてSAとVPPを準備する。`run/ip/vpp` は実コマンドのtimeout、ログ、終了値・VPP CLIエラー検査を担当する。`observe` は実Agent値と連続観測条件、`choose` はC判断、`routes` は両側の実route操作、`trial` は障害・復旧・rollbackと通信観測、`performance` は別枠TCP帯域、`isolation` は重複アドレスのVLAN／FIB分離を担当する。`cleanup` は測定プロセスと試験runtimeを停止する。`evaluate` はmanifest、排他、反復、途中CSV、失敗記録を統括する。

`generate-paper-graphs.py` の `generate_metric_graphs` は各有限値の統計、`write_svg` はグラフ、`generate_evaluation_graphs` は従来のpass／skip等の機能評価図を生成する。`vm-paper-collect-metrics.sh` は評価とグラフ生成の入口であり、測定ロジックを重複実装しない。
