# 文書の読み方

Ibukiの実装・検証・論文提出条件は[現状一覧](current-status.md)を正本とする。設計目標、コード実装、計画生成、実疎通、定量評価、本番運用を区別する。

## 利用と開発

- [Repository README](../README.md): 概要、ビルド、基本操作
- [Linux VM手順](../scripts/README-linux-vm.md): 専用VMでの実通信試験
- [Shell一覧](shell-commands.md): 引数、環境変数、試験の入口
- [YAMLルート](yaml-routes.md): 正規スキーマと制約
- [イベントスキーマ](event-schemas.md): Telemetryと観測入力
- [共同作業ガイド](worker-guide.md): ファイル、処理、出力の対応
- [C関数リファレンス](code-reference.md): ソース関数の説明
- [Namespace Runtime v2](namespace-runtime-v2.md): LAN、VPP、XFRMの配置

## 実装境界と運用

- [Scenarioとeventnetd](scenario-vs-production.md): 模擬入力と常駐入口の差
- [Service配置](eventnetd-service.md): 常駐構成と運用上の注意
- [Transport Adapter](transport-adapter-guide.md): CLIとSDK接続境界
- [VPP Binary API](vpp-api-implementation.md): transportと未実装codec
- [Security監査メモ](security-audit-notes.md): 現行対策と日付付き監査履歴

## 論文と実装計画

- [現状・提出条件](current-status.md): 確認済みの成果、限界、再現手順、提出直前の確認
- [評価チェックリスト](paper-evaluation-checklist.md): 試験項目と評価入口
- [実測プロトコル](live-measurement-protocol.md): 指標の定義、関数責務、測定条件
- [原稿照合](paper-claim-audit-20261010.md): コード・測定・主張の対応
- [期間別ロードマップ](paper-to-mitou-roadmap.md): 期間ごとの目的、優先順位、成果物
- [具体的実装計画](paper-to-mitou-implementation-plan.md): ファイル・責務、受け入れ条件、次段階の実装
- [研究本文](../研究内容.tex)、[実装方針](../実装方針.md): 主張と設計目標

## 測定資料：evaluation/

- [8 vCPU反復結果](evaluation/20261010-8cpu/README.md)
- [旧2 vCPU反復結果](evaluation/20261010/README.md)
- [GRE最終疎通](evaluation/20261010-gre/README.md)

測定CSV・統計・図・秘匿済みログを条件別に保存する。生ログは `out/` 以下のローカル生成物で、鍵を含み得るため公開しない。各CPU条件は25ケース、GRE最終確認は1回の機能疎通であり、同じ評価範囲ではない。

## 研究比較：research/

- [先行研究・SD-WAN 2.0比較](research/prior-research-and-sdwan2.md)
- [ASANO比較](research/asano-comparison.md)

比較資料は研究上の位置付けであり、他方式との性能比較の実測ではない。

## 履歴資料：archive/

- [詳細な到達点と検証履歴](archive/mitou-submission-status.md)
- [旧GRE構成の制約](archive/gre-namespace-constraint.md)
- [GRE作業線の統合記録](archive/gre-data-plane-fork.md)
- [awesome-mitou初期比較](archive/awesome-mitou-comparison.md)

履歴資料は現行仕様の正本ではない。現在の判断は現状一覧、構成はNamespace Runtime v2に従う。旧CI結果はそのcommit・環境に限り、最新コードの合格とは読み替えない。[日誌](../daily/20261010.md)と[txt資料](../txt/README.md)も日付付き記録として扱う。

## 文書の更新規則

通常読む文書はこの階層の18件（索引を含む）にまとめ、履歴・比較・測定資料は別階層へ分離する。現状と提出条件は `current-status.md`、期間の優先順位はロードマップ、コード配置と受け入れ条件は具体的実装計画だけを更新し、同じ一覧を複数文書へ複製しない。

旧 `paper-submission-minimum.md` は現状一覧、旧 `future-implementation-map.md` は具体的実装計画へ統合した。過去の日誌の旧パスは当時の記録として維持する。
