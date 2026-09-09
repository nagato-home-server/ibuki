# 論文提出から未踏期間までの実装計画

## 判断の前提

Ibukiの制御入力は、Web UIが生成する検証済みの正規スキーマを中心にする。YAMLは宣言的な設定の持ち運び、CI、再現実験、バックアップのために残す。したがって、論文提出時点でYAML 1.2全体を実装することは目的にしない。Controllerは入力形式にかかわらず、未知の項目・範囲外の値・未対応機能を拒否する。

JSONLの処理は、入力・出力ともyyjsonを共通ライブラリとして使用する。JSONLの1行区切り、Ibukiスキーマ、鮮度、重複、測定値の妥当性はController側で検証する。yyjson導入による速度比較は本計画の評価項目にしない。

## 論文提出まで

### 必須実装

1. yyjsonによるJSONL入力の実装を維持し、Agent、eventnetd、シナリオ、strongSwan Observer、VPP ObserverのJSON出力もyyjsonで生成する。
2. 現在の固定長配列モデルと上限値を文書化し、上限超過をエラーとして扱う。
3. YAMLはIbukiが定義する正規スキーマを対象に、経路選択、優先度、Explicit、Priority、Evaluated、waypoint、VLAN、VRF、FIB、fallbackを検証する。
4. Agentの逐次測定を基準実装とし、測定結果をControllerへ投入してPath選択へ反映できることを確認する。
5. direct障害、hub fallback、recovery、relay選択、rollback、XFRMポリシー、VLAN/FIB計画を再現可能なテストとして残す。
6. strongSwanは既存のCLI/VICI接続境界、VPPはvppctl経路計画境界を検証対象とし、実環境依存部分はskip理由を出力する。

### 論文提出時点では実装しないもの

- YAML仕様全体への完全準拠。
- 履歴検索を目的とした外部Telemetry DB。
- VPP Binary APIの全メッセージcodec。
- 本番用のHA、BGP/FRR、証明書自動更新、Flow Preserve。

## 論文提出から未踏期間まで

1. Web UI/APIの正規入力を定義し、UI入力をControllerの検証済み内部モデルへ変換する。Controllerでも同じ制約を再検証する。
2. Agentに任意Pathの測定周期、回数、timeout、並列数、送信元を指定できる設定を追加する。
3. 並列測定を実装する。最初はLinuxで上限付きワーカ数の実験モードとして導入し、逐次モードをフォールバックとして残す。1ラウンドの測定開始時刻を揃え、結果に測定ラウンドIDと開始・終了時刻を付ける。
4. 閾値、ヒステリシス、hold-down、recovery条件を設定ファイルから変更できることを確認し、切替回数と誤切替を測定する。
5. Telemetry保存の抽象インターフェースを追加する。論文提出時はJSONLファイルを標準実装とし、必要になった場合だけSQLite等を差し替えられる形にする。
6. VPP Binary APIとstrongSwan VICIを実機に接続するための最小adapterを完成させ、vppctl/CLIは診断用・互換用として残す。

## 未踏期間中

1. 複数VM、物理機、クラウド拠点を含む実験環境を構築し、Direct、Hub、Relay、クラウドIPsecの切替を実測する。
2. Telemetry DBを導入する。用途は履歴ダッシュボード、長期傾向、閾値調整、障害解析、イベント再生、複数Agentの時系列相関であり、Path選択そのものに必須ではない。小規模構成はSQLite、時系列・多拠点構成はPrometheus等を候補とする。
3. 証明書認証、鍵更新、失効確認、認証情報の安全な格納をstrongSwan VICI操作と結合する。
4. VLAN/VRF/FIBをVPP Binary APIで実反映し、IPsec対象外通信の遮断とrollbackを実トラフィックで検証する。
5. BGP/FRR連携、クラウド能力プロファイル、Controller HA、再起動復旧を追加する。
6. Flow Preserveを、既存フローの識別、二重経路、切替中の状態保持、失敗時rollbackの順に実装する。
7. 並列測定を本番モードへ昇格し、CPU・ソケット・同時数の上限、過負荷時の抑制、測定失敗時の再試行を実装する。

## 一般YAML完全対応の扱い

一般YAML完全対応は不要である。Web UI/APIを主経路にする場合、重要なのはYAMLの文法網羅率ではなく、正規スキーマの互換性、エラー表示、再現性、安全な拒否である。ただし外部利用者がYAMLを直接編集する場合に備え、対応キー、型、既定値、上限、未対応キーの扱いを明示し、将来libyaml等へ置換できるParser境界を維持する。

## 並列測定の採否

並列測定は実装する。複数Pathを順番に測ると測定時刻がずれ、障害発生時の比較が不公平になるためである。一方、無制限並列はAgent自身が回線を圧迫するので、既定値は安全な小さい並列数、設定可能な最大値、timeout、キャンセル、逐次フォールバックを必須とする。論文では逐次基準と並列実験モードの差を評価し、速度向上そのものではなく測定時刻の整合性と選択結果の再現性を示す。
