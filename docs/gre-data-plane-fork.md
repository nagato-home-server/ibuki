# GREデータパス フォーク

このフォークは、GREの実データパスを段階的に成立させるための作業線です。

## 第一段階

`scripts/vm-gre-vpp-data-smoke.sh` が、コントローラ生成のVPP経路計画を適用した後、VPP内に往路と復路のGREトンネルを作成し、サイト間のLAN通信を実パケットで検証します。

```sh
sudo sh scripts/vm-gre-vpp-data-smoke.sh samples/gre-vpp-data-plane.yaml
```

この試験では、VPPがroot名前空間で動作し、サイト名前空間にはVPP host-interfaceを接続する既存のVMトポロジを使います。GREの外側はVPP host-interfaceの対向であるサイト側アドレスを使用します。VPPの計画生成は `samples/gre-vpp-data-plane.yaml` から行います。

## 成功条件

- `gre0` が site-a から site-b への外側宛先を持つ
- `gre1` が site-b から site-a への外側宛先を持つ
- 両方向のLAN pingが成功する
- VPPのGREインターフェースがupになる

## strongSwanとの接続

第一段階はVPP GRE単体のデータパス確認であり、暗号化を成功条件に含めません。既存のGRE over IPsec試験は、strongSwan/XFRMをサイト名前空間、VPP GREをroot名前空間に置いているため、IKE/CHILD_SA確立と実パケット転送を同時に成立させるには名前空間境界の再設計が必要です。

次段階では、次のいずれかを選択します。

1. VPPとstrongSwanを同一名前空間で動作させる
2. XFRM interfaceをroot名前空間へ公開する
3. VPP Native IPsecで暗号化し、strongSwanは鍵・SA管理に限定する

このフォークではまず暗号化前のGRE経路を固定し、その後に暗号化バックエンドを差し替えます。
