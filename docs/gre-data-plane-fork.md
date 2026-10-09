# GREデータパス作業線の統合

GREの実データパス開発はmainのnamespace v2ランタイムへ統合済みであり、独立したフォーク構成を利用する必要はない。この文書は旧作業線からの案内を残す。

## 現行の入口

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

VPPとstrongSwanは同じsite namespaceで動作し、独立したclient間のLAN通信をVPP GREとLinux XFRMで転送する。両siteのGRE interfaceはそれぞれのVPP内のgre0である。2026-10-09に双方向疎通、ESP進行、終了後cleanupを確認した。GREはL3トンネルであり、L2延伸ではない。

## 比較用Native実験

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-vpp-native.yaml
```

こちらはGREではなく、静的SAを使用するVPP IPIP/IPsec。過去のCIに成功記録があるが、今回のVM再検証対象ではない。IKE／SA同期／鍵更新の本番連携は未完了である。

構成は[Namespace Runtime v2](namespace-runtime-v2.md)、確認範囲・測定制限は[現状一覧](current-status.md)、旧root VPP構成の問題は[移行前の記録](gre-namespace-constraint.md)を参照する。
