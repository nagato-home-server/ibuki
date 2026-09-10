# ASANO Systemとの方式比較

## ASANOの方式

東京大学情報基盤センターの公開資料に記載されたASANO System Version 1.0は、IPsecに加えてVXLAN、EVPN、DMVPN、NHRPなどを組み合わせたオーバーレイネットワークである。特にDMVPNはmGREとNHRPを利用する構成であり、ASANO v1をIPsec VTI単体のシステムと分類するのは正確ではない。ASANO v2ではVPN部分にWireGuardを導入し、スター型構成へ変更している。

参考：<https://www.itc.u-tokyo.ac.jp/Annual_Report/no20/AnnualReportNo20_v20231012.pdf>

## Ibukiの方式

IbukiはASANOのL2延伸機能を再実装しない。標準対象は、拠点間のIP prefixを経路制御するL3ネットワークである。論文および未踏期間の実装では、GRE over IPsecをL3トンネル方式の第一候補とする。

```text
拠点LAN
  ↓ L3 route
GRE tunnel
  ↓ IPsecで暗号化
WAN
```

GREはL3パケットを仮想point-to-point経路へ収容するため、将来のBGP/FRRや複数のL3経路をトンネル上で扱いやすい。一方、GRE自体は暗号化しないため、実運用ではIPsecを外側に配置する。VTIはGREヘッダを追加せず、IPsecを仮想インターフェースとして扱うroute-based VPN方式であり、単純なL3 site-to-siteでは実装負担とオーバーヘッドを抑えられるため、将来の代替Backendとして残す。

## 比較上の境界

ASANO v1との比較では、ASANOがL2延伸とオーバーレイ制御を含むのに対し、IbukiはL3 Path Selection、Telemetry、状態遷移、IPsec、VPP route制御に集中する。VXLAN、EVPN、L2 bridge、VTEP、VNI、mGRE/NHRPによるL2延伸はIbukiの実装対象外である。したがってIbukiの優位性はL2機能の網羅ではなく、ベンダや回線を限定しないL3制御、Web/API連携、OSSとしての拡張可能なAdapter構造に置く。
