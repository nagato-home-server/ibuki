# 入力境界修正後のVM主要疎通とAgent FD調査

## 対象と条件

2026-10-10、基準HEAD `e168a0d` にAgent FD修正と回帰試験追加を加えたコードで確認した。既存の入力境界・経路選択修正も含む。VM共有フォルダのソースから再ビルドし、試験開始前に採取したソース・ヘッダー・YAML・試験スクリプトのSHA256が終了時に一致することを確認した。

Ubuntu VM、Linux 7.0.0-38-generic、8 vCPU、VPP26.06-release。`VPP_NS_POLL_SLEEP_USEC=1000`。ビルドは`vm-build-cc.sh`、統合試験は`vm-real-smoke.sh samples/linux-vm-netns.yaml integrated`、GREは`vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml`を使用した。GREの再ビルドは省略したが、直前に同じソースでビルド済み。

## VM結果

| 確認項目 | 結果 |
| --- | --- |
| Cビルド・C unit | 成功 |
| Agent | 模擬・YAML展開・実ping・不正引数・出力保護試験が成功 |
| L3 underlay | Direct／Hub／Relay成功 |
| Direct IPsec＋VPP | 双方向各3/3、損失0%、平均RTT往路6.364ms・復路6.875ms |
| Hub Fallback IPsec＋VPP | 双方向各3/3、損失0%、平均RTT往路7.104ms・復路6.454ms |
| GRE over IPsec | 双方向各3/3、損失0%、平均RTT往路5.478ms・復路6.295ms |
| 暗号化確認 | DirectのESPカウンタ増加、Hub両区間のESP送信各0→3、GREのESP送受信sequence進行を確認 |
| ソース整合性 | 試験前後のSHA256照合成功 |
| cleanup | site-a/site-b/hub-1のプロセス0、XFRM state/policy各0、システムVPP active |

Hub Fallbackは障害イベント指定による選択確認であり、今回の再確認で物理リンク切断の性能評価を再取得したわけではない。RTTは各3pingの疎通確認値であり、5回反復の性能統計や旧測定値の置換に用いない。VPP CLI・strongSwan/XFRM経由の現行構成であり、VAPI有効分岐や長時間rekeyを確認したという意味ではない。

最初のAgent試験はHGFS共有フォルダ上の出力ファイルを安全でない権限として拒否した。権限検査を弱めず、VM内の`/run`へAgent試験出力を置いて再実行し成功した。共有フォルダへ鍵・Telemetryファイルを安全に保存できることを保証しない。

## AgentのFD警告

元の2件は`dup2`でstdout/stderrをpingへ引き継ぐ経路に対するGCCの漏洩警告だった。exec成功時はpingが使用し、exec/dup2失敗時は`_exit`によりOSが解放するため、調査した経路で親のFD漏洩は確認しなかった。

一方、標準FDが閉じた起動条件ではpipeがFD 0/1/2を再利用し、旧コードの`close(pipe_fds[1])`が接続済みstdout/stderrを閉じる問題を再現した。stdin/stdoutを閉じた旧コードでは、成功するping fixtureでも測定失敗になった。書込FDを`F_DUPFD_CLOEXEC`で3以上へ複製してから元のpipeを閉じ、stdout/stderrへ接続するよう修正した。

`tests/test_agent_process.c`とPython runnerで標準FDの開閉8組合せ、stdout成功・stderr成功・ping終了失敗・不正RTT・実行ファイル不在の5条件を各30回、計1200 probe確認した。全40ケース成功、各回の親FD数は一定、終了子プロセスの未回収もなし。Linux Clang ASan/UBSan・leak検査付きCTest30/30、Windows MSVC CTest28/28成功。

修正後のAgent本体はGCC/Clang analyzerとも警告なし。GCCは標準FDを意図的に閉じる回帰harnessについて、dup2の接続先FD 1が無効かもしれないという別の警告を残す。dup2は閉じた接続先にも複製できるため元の漏洩警告と区別し、harnessのGCC記録はreviewのまま保持する。Clangではharnessも警告なし。任意のリソース枯渇・fork失敗・長時間運用を網羅した証明ではない。

## ローカル証拠

未加工ログにはIPsec鍵を含み得るため公開しない。Git対象外の次の場所に保持する。

- `out/vm-recheck-20261010/`: summary.csv、各試験ログ、environment.txt、sources-before.sha256、source-consistency.log、cleanup-check.log。
- `out/agent-fd-gcc/`、`out/agent-fd-clang/`: 静的解析とソースSHA256。
- `out/agent-fd-old-reproduction.log`: 旧コードで閉じた標準FD条件が失敗する記録。
- `out/agent-fd-sanitizers-tests.log`、`out/agent-fd-windows-tests.log`: CTest結果。

主要ファイルSHA256:

| ファイル | SHA256 |
| --- | --- |
| `examples/eventnet_agent.c` | `74875d429c3d5c9d3218bb028196b3ec38de3acebc45dee339dc9badf66b581a` |
| `src/telemetry.c` | `36bdd0b0a65d05a41ad8fd05b62fac79516a459e23fc9915abd44f48ca89936a` |
| `src/yaml_config.c` | `b1f1c63eee0c66fa48c7702ce4cf4fe928049df3fcba7ff2f16665a0c529153c` |
| `samples/linux-vm-netns.yaml` | `2336e16ff14afbddf04a44b1a3df507ef1ae1f84e16a6375bc3cd8679989aa19` |
| `samples/gre-namespace-v2.yaml` | `93f41ac5a16c659e560a01ff4f24725a693d9f8ea13916883eaf4a27f1ae62d7` |
