# strongSwanを残す理由

VPP Native IPsecの話が出たとき、Ibukiの設計会議は一度、別の方向へ傾きかけた。

「VPPでGREもIPsecも処理できるなら、その方が速いのではないか」

確かに、VPPは高速なパケット転送とFIBを一つのデータプレーンで扱える。GREとIPsecを同じ場所で処理できれば、VPPの外へパケットを渡す必要もなくなる。性能だけを見れば、魅力的な選択肢だった。

しかし、Ibukiが作ろうとしているものは、IPsec装置そのものではない。

strongSwanには、すでにIKE、認証、鍵交換、再鍵交換、CHILD_SA、失効処理といった長い運用の蓄積がある。VPP Native IPsecへ移るなら、暗号化処理を置き換えるだけでは済まない。strongSwanが管理するSAをVPPへ同期し、SPIと鍵の対応を保ち、再鍵交換や障害時の状態を追い、失敗した設定を逆順に戻さなければならない。

その境界をIbuki自身が持ち始めると、研究の中心が変わってしまう。

経路を選ぶこと。状態を観測すること。新しい経路を準備し、検証し、切り替え、失敗したら戻すこと。Ibukiが明らかにしたいのは、その制御の手順と責任分界である。暗号処理や鍵交換の実装を増やすことではない。

そこで、論文提出までの仕様を決め直した。

```text
strongSwan ：IKE、認証、IPsec SA、暗号化の制御
Linux XFRM：Linux評価環境のIPsecデータプレーン
VPP        ：route、VRF、VLAN、forwarding
Ibuki      ：Path Selection、Telemetry、Transition、Rollback
```

Linux XFRMを使うことは、Linuxの実装をIbukiへ取り込むことではない。strongSwanのAdapterを通して、OSが提供するIPsec機能を外部Backendとして利用するだけである。FreeBSDならPF_KEY、別の環境なら別のkernel interfaceが選ばれる。Ibukiが固定するのは、OS内部の仕組みではなく、Tunnelが確立したか、経路が存在するか、通信が検証できたかという観測の境界である。

GREは捨てなかった。VPP GREの設定生成と、strongSwanでGREを保護する計画は、将来の検証材料として残した。ただし、VPP GREのパケットをLinux XFRMへ渡す仕組みを、検証なしに正式Backendとは呼ばないことにした。

論文の後、未踏期間にVPP Native IPsecを独立したBackendとして調べる。そのときは、strongSwanを制御プレーンとして残せるのか、SAをどこで管理するのか、VPPとどのように同期するのかを先に決める。FRRoutingのBGPやOSPFも、その実データパスが確認できてから接続する。

速いから採用するのではない。Ibukiの責任範囲を守りながら、速さを測れる状態を作る。

その判断によって、仕様は小さくなったのではない。何をIbukiが証明し、何を既存OSSへ委ね、何を将来の比較対象として残すのかが、初めて明確になった。
