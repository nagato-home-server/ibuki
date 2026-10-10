# awesome-mitou Comparison

履歴資料: 現在の仕様・完了条件は[現状一覧](../current-status.md)を正本とする。この文書は過去の経緯や詳細説明を保存するための資料である。

初期比較日: 2026-07-29。本文の比較は当時のcheckoutを対象とした主観的な開発メモで、現在の両repositoryの優劣や機能一覧ではない。

## 2026-10-10の追記

Ibukiには現在、GitHub Actions、eventnetdの1回/周期/socket入力、Telemetryスキーマ、secret権限と入力検証、公開測定CSV・図がある。初期の「CIがない」「eventnetdがない」「configが644」は解消済み。現在残る課題は本番運用、Agent並列測定、API codec、テストの分割・保守性である。[現状一覧](../current-status.md)を正とし、以下は過去の参考・改善案として読む。

## 初期比較の記録

比較対象:

- local `awesome-mitou` checkout
- this repository

この文書は、`awesome-mitou` のリポジトリ構成・実装姿勢・見せ方を読み、PathWeaver / EventNet controller C 実装と比較したメモです。

## 1. 前提

`awesome-mitou` は、未踏応募資料や成果報告書へのリンクを集めるリポジトリです。

そのため、ネットワーク制御基盤である PathWeaver と機能分野はまったく違います。
ただし、OSSとしての完成度、未踏提出時の説得力、第三者が読める構成という意味では参考になります。

## 2. awesome-mitou の強いところ

### 2.1 入口が明快

`awesome-mitou` はREADMEを開いた瞬間に、何のリポジトリか分かります。

- 未踏事業の資料リンク集である。
- 年度・事業区分・プロジェクトごとに整理されている。
- コントリビュート方法がある。

PathWeaverもREADMEに現状は書けていますが、まだ「何を1分で見ればすごさが伝わるか」は弱いです。

### 2.2 CIが整っている

`awesome-mitou` は GitHub Actions で以下を回しています。

- lint / format
- typecheck
- test coverage
- dead link check
- scheduled discovery
- candidate PR automation

これは「継続して動くプロジェクト」であることを示しています。

PathWeaverはローカル/VM smoke scriptはかなり増えていますが、GitHub Actions上で動く軽量CIがまだありません。

### 2.3 純粋関数とI/O分離が強い

`awesome-mitou` は、core logicと外部I/Oが分かれています。

- `src/core/*`
  - scoring、classification、rendering、pipelineなどの純粋処理。
- `src/infra/*`
  - GitHub CLI、clock、logger、storeなどの外部依存。
- `src/cli/*`
  - CLI引数、main、discover orchestration。

この分離により、unit testが書きやすくなっています。

PathWeaverも以下の分離はあります。

- `src/path_selection.c`
- `src/controller.c`
- `src/transition.c`
- `src/yaml_config.c`
- `src/render_commands.c`
- `src/apply_plan.c`
- mock adapters

ただし、runtime生成やscenario harnessにはまだI/Oと判断ロジックが混ざっています。

### 2.4 テストの粒度が細かい

`awesome-mitou` はunit / integration / fixtures / snapshotが分かれています。

- scoring
- classification
- render
- pipeline
- URL extraction
- repo-name parsing
- retry
- CLI integration

初期のCテストは`tests/test_controller.c`に集中していた。現在もCテストの分割は課題で、Python計測器のテストは別ファイルに追加済み。
これは初期実装としては十分ですが、今後は以下のように分けると読みやすくなります。

- `tests/test_path_selection.c`
- `tests/test_yaml_config.c`
- `tests/test_render_commands.c`
- `tests/test_apply_plan.c`
- `tests/test_scenario.c`
- `tests/test_security_validation.c`

### 2.5 入出力契約が明示されている

`awesome-mitou` は `zod` schemaでJSON出力契約を定義しています。

PathWeaverも `selected-path.txt`、Explain JSONL、runtime shell scriptはあります。
ただし、JSON schemaや出力契約の明文化はまだ弱いです。

未踏提出では、`status.jsonl` / `explain.jsonl` のschemaを1枚で示すと強くなります。

## 3. PathWeaver の強いところ

### 3.1 実機/VMで動くネットワーク制御まで行っている

PathWeaverは単なるCLIや資料ではなく、Linux VM上で以下を確認しています。

- network namespace underlay
- direct IPsec
- hub IPsec
- VPP forwarding
- controller-generated VPP route
- IPsec + VPP integrated runtime
- direct failure fallback
- scenario harness

これは `awesome-mitou` より実験系として重いです。

### 3.2 設計と実装の接続がある

PathWeaverは、設計文書の構成に沿って以下を実装しています。

- Intent
- Path
- Tunnel
- Segment
- VPP edge
- Health
- Transition
- Explain
- Adapter bridge

設計構想とCコードが対応している点は強いです。

### 3.3 Cで実装している意味がある

strongSwan / VPP / FRRouting とつなぐ将来を考えると、C実装は説得力があります。

特に、未踏審査では「OSSをただshellで呼ぶだけではないのか」という疑問が出やすいです。
ここに対して、Cでcontroller model / path selection / transition / adapter boundaryを持っているのは強みです。

## 4. PathWeaver の弱いところ

### 4.1 GitHub上で一発で信頼できる状態になっていない

現状はVMでかなり動いています。
ただし、第三者がGitHubだけ見たときの信頼材料が不足しています。

不足しているもの:

- GitHub Actions CI
- test badge
- minimal non-root test
- security smoke
- generated demo artifact
- screenshot / diagram
- release-like milestone

### 4.2 デモ経路が多く、最短ルートが見えにくい

scriptsが多いのは実装が進んでいる証拠です。
一方で、初見の人には「どれを実行すればよいか」が迷いやすいです。

必要な整理:

- `Quick Demo`
  - 非rootで必ず通る。
- `VM Full Demo`
  - root/VPP/strongSwanあり。
- `Submission Demo`
  - 審査員に見せる順番。

### 4.3 security postureがまだ荒い

`docs/security-audit-notes.md` に書いた通り、現時点では以下が不安です。

- YAML由来値のcommand injection
- `system()` 実行
- PSKの平文保存
- `chmod 644` のsecret config
- secret入りconfigのログ出力
- YAML値のサイレント切り詰め
- `atoi()` / `atof()` の緩いparse

未踏提出では「研究開発中のprototype」として許容され得ますが、ネットワーク制御基盤を名乗るなら、ここを少し直すだけで説得力がかなり上がります。

### 4.4 テストが大きな1ファイルに寄っている

`tests/test_controller.c` はすでに価値があります。
ただし、今後の作業者には粒度が大きく見えます。

`awesome-mitou` のように、領域ごとに小さなtest fileへ分けると、品質が伝わりやすくなります。

### 4.5 eventnetd がまだない

PathWeaverの設計上の主役は、最終的には常駐/準常駐controllerです。

現状はscenario harnessとruntime generatorが強い一方、`eventnetd --once` がまだありません。
これは本番controllerに近づけるうえで最重要の差分です。

## 5. 比較表

| 観点 | awesome-mitou | PathWeaver 現状 | PathWeaver の次 |
| --- | --- | --- | --- |
| READMEの分かりやすさ | 強い | 中 | 図と最短demoを追加 |
| CI | 強い | 弱い | GitHub Actions追加 |
| Unit test | 強い | 中 | test file分割 |
| Integration test | 中 | 強い | VM結果をdocs化 |
| 実機価値 | 低〜中 | 強い | demo動画/図で伝える |
| 型/契約 | 強い | 中 | JSONL schema化 |
| Security posture | 中 | 弱〜中 | PSK/system/validation対策 |
| 自動化 | 強い | 中 | demo script整理 |
| 未踏向け説得力 | 資料集として強い | 技術実証として強い | 見せ方と安全性を補強 |

## 6. 未踏提出までに真似すべきこと

優先度順:

1. GitHub Actionsで `vm-build-cc.sh` 相当の非root build/testを回す。
2. README冒頭に「30秒で分かる成果」を置く。
3. `demo-mitou.sh` の出力例をREADMEに貼る。
4. `eventnetd --once` を作り、scenario harnessから本番controller入口へ寄せる。
5. `docs/security-audit-notes.md` の最優先項目を2〜3個直す。
6. `Explain JSONL` のschemaを明文化する。
7. テストを分割する。
8. Mermaid図で `YAML -> Controller -> Path Selection -> strongSwan/VPP -> Explain` を示す。

## 7. すぐ直すなら

最初の一手としては、以下が最も費用対効果が高いです。

### 7.1 CI追加

追加するもの:

- `.github/workflows/ci.yml`

内容:

- Ubuntu runner
- `sudo apt-get install -y build-essential`
- `sh scripts/vm-build-cc.sh`
- `sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml`

VPP / strongSwan実行はCIではやらず、非rootで通る範囲に限定します。

### 7.2 README冒頭の見せ方改善

README冒頭に以下を足します。

- 何を作っているか
- 何が動いたか
- どう実行するか
- 未踏提出で何を見せるか

### 7.3 security quick fix

まず直すもの:

- `swanctl.conf` を `chmod 600`
- secretをログに出さない
- `PSK=change-me` を警告または拒否

## 8. 判断

`awesome-mitou` と比べると、PathWeaverは技術的な深さと実機実証では勝っています。

一方で、初見の読みやすさ、CI、テスト粒度、出力契約、安全性の見せ方では負けています。

未踏提出向けには、今から大きな機能を増やすより、以下を整える方が効果が大きいです。

- `eventnetd --once`
- CI
- READMEの最短demo
- security quick fix
- 図
- test分割

この6点を入れると、「動く研究実装」から「第三者に見せられるプロトタイプ」へ近づきます。
