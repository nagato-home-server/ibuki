# GRE over IPsecの旧namespace構成制約

この文書はroot namespaceにVPPを共有配置していた時期の検証記録であり、現在の構成を示すものではない。現在は拠点ごとのVPPとstrongSwanを同じnamespaceに置く。構成、アドレス、検証状態は[Namespace Runtime v2](namespace-runtime-v2.md)を参照する。

## 概要

GRE over IPsecの実験では、strongSwanのIKE_SA確立とVPP GRE interface生成までは成功したが、GREを経由したLAN間通信は成立しなかった。原因は、VPPとLinux XFRMを異なるnetwork namespaceに配置した現在の評価構成にある。

## 現在の配置

```text
site-a namespace                         site-b namespace
  203.0.113.10  -- direct underlay --      203.0.113.9
  172.16.1.2       vpp-client               172.16.2.2
       |                                      |
       |                                      |
root namespace: VPP
  host-vpp-site-a 172.16.1.1       host-vpp-site-b 172.16.2.1
```

- strongSwanのcharonとLinux XFRM state／policyは`site-a`と`site-b` namespaceで動作する。
- VPP daemonとGRE interfaceはroot namespaceで動作する。
- VPPは`172.16.1.1`から`172.16.2.1`へGRE packetを送る。
- GREの復号後packetをVPPへ戻すためには、対向namespaceのXFRM処理とVPP host interfaceを同じforwarding pipelineへ接続する必要がある。

## 観測結果

1. `gre-a-b`のIKE_SAは`203.0.113.10`と`203.0.113.9`の間で確立した。
2. VPPの`gre0`は生成された。
3. GRE outer endpointをIKE endpointと同じ値にすると、CHILD_SAは確立するが、復号後packetがsite-b namespaceのローカルunderlay側で終端する。
4. GRE outer endpointをVPP host interfaceの`172.16.1.1`／`172.16.2.1`にすると、VPPへ戻す経路は表現できるが、strongSwanのtransport selector交渉で`TS_UNACCEPTABLE`となった。
5. したがって、現在の実装で確認できたのは、設定生成、namespace内charon起動、VICI接続、IKE処理、VPP GRE生成までである。

## 設計上の結論

これはplugin不足やPSK認証の失敗ではなく、VPPとXFRMの責任境界をnamespace間で正しく接続できていない構成問題である。Direct／HubのLinux XFRM runtimeと、root namespace VPPの通常L3 forwardingは別々に成立しているため、GRE over IPsecの失敗をIbukiのPath Selection失敗とは扱わない。

論文提出時点では、GRE over IPsecを正式な実データパス評価から外し、次の範囲に限定して報告する。

- YAMLからGRE over IPsec planを生成できる。
- namespace内strongSwanをVICIから操作できる。
- GRE transport modeの設計とselector生成を検証できる。
- VPP GRE interfaceとrouteの生成境界を確認できる。
- VPP GRE packetがIPsecで保護されてLAN間を通過することは未検証である。

## 解決案

未踏期間以降に実データパスを完成させる場合は、次のいずれかを採用する。

1. VPPとstrongSwan／XFRMを同一namespaceまたは同一host forwarding domainに配置する。
2. Linux XFRM interfaceをroot namespaceへ公開し、VPPとXFRM interfaceを明示的に接続する。
3. VPP Native IPsecを採用し、VPP内でGRE outer packetの暗号化・復号を完結させる。
4. IKE endpoint、GRE outer endpoint、XFRM selector、VPP host interfaceの対応をYAMLで明示し、経路検証を追加する。

現時点では、2または3がIbukiのVPP連携を本番化するうえで自然である。ただし、VPP Binary APIの版固定と、strongSwanとの責任分界の検証が必要になる。
