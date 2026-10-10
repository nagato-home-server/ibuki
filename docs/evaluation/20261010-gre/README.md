# GRE最終疎通確認（2026-10-10）

## 確認条件

Ubuntu VM、8 vCPU、メモリ割当8 GB（OS表示7.7 GiB）、Linux 7.0.0-38-generic、VPP26.06-release、strongSwan5.9.13。専用namespaceを作り直し、既存バイナリで生成計画を実行した。今回はC sourceを変更しておらず、フルビルドを省略した。

```sh
sudo SKIP_BUILD=1 OUT_DIR=out/gre-final-new/plan sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

既存のIPsec/VPP試験を停止した専用VMが前提。今回の実行ではシステムVPPを一時停止し、終了後に再起動した。

## 結果

| 対象 | 結果 |
| --- | --- |
| client-a → client-b | 3送信／3受信、損失0%、平均RTT 5.516ms |
| client-b → client-a | 3送信／3受信、損失0%、平均RTT 5.306ms |
| site-a ESP送信／受信 | oseq=0x8、seq=0x7 |
| smoke終了値 | 0 |
| 終了後両siteのプロセス | なし |
| 終了後両siteのXFRM state／policy | 各0行 |
| システムVPP | activeへ復帰 |

ESP sequenceはneighbor warm-upを含むため、各3回の測定pingに対応する増分としては扱わない。両方向の初回warm-upは測定pingから除外した。任意pluginのロード警告は残ったが、使用したIKE/ESP/GRE経路の疎通は成功した。警告だけから全機能の安全性を保証するものではない。

これはVPP GRE→Linux XFRMで暗号化するL3構成。Native VPP IPIP/IPsecの再検証、GRE帯域・障害切替の5回反復、MTU限界、長期rekey、L2延伸の試験ではない。

## 証拠と識別子

- [runtime-sanitized.log](runtime-sanitized.log): 鍵と共有フォルダ絶対パスを除去した実行ログ。未加工ログはgit対象外の `out/gre-final-20261010/runtime.log` に保持する。
- YAML SHA256: `93f41ac5a16c659e560a01ff4f24725a693d9f8ea13916883eaf4a27f1ae62d7`。
- smoke script SHA256: `41a74611021870e6bd069964c1dbe9d33bca62bc67fa516e782d7b435e40dd87`。
- 使用バイナリ `eventnet_netns_plan` SHA256: `2d97e0521681542e1834d39069fcec2f11855b73b5c880a8a1ed2bf13777f617`。
- 基準HEAD `806bc6b` と未コミットの計測・原稿変更。過去CIの成功と今回のVM結果を混同しない。
