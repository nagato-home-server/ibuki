# GREデータパス フォーク

このフォークは、GREの実データパスを段階的に成立させるための作業線です。

## 第一段階

旧root名前空間向けのVPP GRE smokeは整理し、現在はnamespace v2の統合入口で検証します。コントローラ生成の経路計画、VPP Native IPsecまたはstrongSwan、サイト間LAN通信を一つの実験入口から扱います。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-vpp-native.yaml
```

この試験では、サイト名前空間ごとにVPPを起動し、VPP Native IPsecではstrongSwanを起動せず、VPPのSAとIPIP保護を使用します。strongSwan/XFRMを検証する場合は `samples/gre-namespace-v2.yaml` を入力にします。

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
