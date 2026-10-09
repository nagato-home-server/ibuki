# 文書の読み方

Ibukiの実装・検証状況は[現状一覧](current-status.md)を入口にする。設計目標、実装済みの制御ロジック、計画生成、実疎通、定量評価を区別する。

## 利用と開発

- [Repository README](../README.md): 概要、ビルド、基本的な使い方
- [Linux VM手順](../scripts/README-linux-vm.md): 専用VMでの実通信試験
- [Shell一覧](shell-commands.md): 引数、環境変数、試験の入口
- [YAMLルート](yaml-routes.md): 対応する正規スキーマと制約
- [イベントスキーマ](event-schemas.md): Telemetryと観測入力
- [共同作業ガイド](worker-guide.md): ファイル、処理、出力の対応
- [C関数リファレンス](code-reference.md): ソース関数の説明
- [Namespace Runtime v2](namespace-runtime-v2.md): 現行LAN、VPP、XFRMの配置

## 実装境界と運用

- [Scenarioとeventnetd](scenario-vs-production.md): 模擬入力と常駐入口の差
- [Service配置](eventnetd-service.md): 常駐構成と運用上の注意
- [Transport Adapter](transport-adapter-guide.md)、[VPP Binary API](vpp-api-implementation.md): CLIの実疎通とSDK接続境界の区別
- [Security監査メモ](security-audit-notes.md): 権限、入力、秘密情報の注意

## 研究と期間別計画

- [提出前の最低条件](paper-submission-minimum.md)、[評価チェックリスト](paper-evaluation-checklist.md): 残る反復・障害・通信品質評価
- [期間別ロードマップ](paper-to-mitou-roadmap.md)、[実装計画](paper-to-mitou-implementation-plan.md): 論文前、論文後、未踏期間
- [今後の実装場所](future-implementation-map.md): 追加するコードの境界
- [実装到達点詳細](mitou-submission-status.md): 機能の説明と日付付き検証履歴
- [研究本文](../研究内容.tex)、[実装方針](../実装方針.md): 主張と設計目標

## 過去の記録

[旧GRE構成の制約](gre-namespace-constraint.md)はroot VPP時代の失敗記録であり、現行仕様ではない。[GRE作業線](gre-data-plane-fork.md)はmain統合後の案内である。`daily/`と[txt資料](../txt/README.md)の日付付き内容は過去の検討として保存する。旧CI結果はそのcommit・環境に限り、最新コードの合格とは読み替えない。

2026-10-09の変更は[日誌](../daily/20261009.md)に記録した。今回の実測ログは`out/`以下のローカル生成物であり、Gitに同梱しない。秘密鍵・SA鍵を含み得る生ログは公開前に秘匿する。
