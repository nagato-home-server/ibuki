# Namespace Runtime v2

更新日: 2026-10-10。独立client LAN、site内VPP/charon、Direct/Hub TAP、GRE underlayが標準。GRE最終双方向疎通とcleanupは[証拠](evaluation/20261010-gre/README.md)を参照。CLI socketとBinary API socketは別protocol。

## 目的

GRE over IPsecの実データパスでは、VPPがroot名前空間、strongSwanがサイト名前空間に分かれている構成を採用しない。各拠点のデータプレーンプロセスを同じネットワーク名前空間へ置き、GREの外側パケットとXFRM処理を同一の経路に通す。

## 新しい配置

```text
client-a namespace (10.10.1.2/24)
  eth0 -- veth -- site-a VPP LAN (10.10.1.1/24)
site-a namespace
  strongSwan/charon-a
  VPP-a (CLI/API socket: /run/ibuki-vpp-ns/site-a/)
  underlay interface
  LAN attachment

client-b namespace (10.10.2.2/24)
  eth0 -- veth -- site-b VPP LAN (10.10.2.1/24)
site-b namespace
  strongSwan/charon-b
  VPP-b (CLI/API socket: /run/ibuki-vpp-ns/site-b/)
  underlay interface
  LAN attachment
```

VPPをサイト単位に分けることで、VPPのGRE outer endpoint、Linux kernelのunderlay、strongSwanのXFRM policyが同じ名前空間で解決される。生成計画は`vpp_edges[].vpp_socket`をnode別CLI接続先として使う。`vpp_api_socket`は現行parserでは同じfieldへの別名であり、CLIとBinary APIのprotocolを自動切替しない。標準sampleにはCLI socketを指定し、API transportは別設定・任意SDKビルドで扱う。

## 共通ランタイム

`scripts/vm-vpp-ns-runtime.sh` は、サイト名前空間ごとにVPPを起動・停止・状態確認する。各インスタンスには個別のCLI socket、API socket、統計用`stats.sock`、PID、ログ、API segment prefixを割り当てる。統計ソケットも分離することで、複数VPPが共有`/run/vpp/stats.sock`をbindして起動に失敗することを防ぐ。

```sh
sudo sh scripts/vm-vpp-ns-runtime.sh start
sudo sh scripts/vm-vpp-ns-runtime.sh status
sudo sh scripts/vm-vpp-ns-runtime.sh stop
```

`scripts/vm-vpp-ns-topology.sh` は独立clientのLAN veth、site内VPP host-interface、Linux underlay peerを作成する。`VPP_TOPOLOGY_MODE=direct|hub|all`では専用VPP transitを作り、`ipsec`では平文transitを作らない。Direct／Hub IPsec統合はTAPからLinux/XFRMへ渡す。GRE統合は`SKIP_GRE=1 SKIP_IPSEC_TAP=1`としてGREを生成計画だけで作る。`scripts/vm-gre-namespace-v2-smoke.sh`は通常Controllerをビルドしてから構成・計画・strongSwanを適用する。事前ビルド済みなら`SKIP_BUILD=1`で省略できる。Git切替後に古いobjectを疑う場合は`FORCE_REBUILD=1 BUILD_DIR=build-ns-v2 sh scripts/vm-build-cc.sh`を使う。共有root VPPのsocket省略動作は互換用であり、標準試験と混同しない。

VPP起動は既定120回のready確認を行い、`VPP_READY_ATTEMPTS`で変更できる。試験用の`VPP_NS_POLL_SLEEP_USEC`は既定1000で、2 vCPU環境の競合を緩和する。性能比較ではpoll設定と同居daemonを記録する。120回はCLI実行時間も含むため、厳密な120秒deadlineではない。

## 移行順

1. サイトごとのVPPプロセスを起動する
2. VPPごとのunderlay/LAN interfaceを作成する
3. VPPごとのGRE往復トンネルを作成する
4. strongSwanを同じサイト名前空間で起動する
5. XFRM interfaceまたはVPP Native IPsecを選択する
6. Controller生成計画からノード別socketへ適用する
7. 双方向LAN ping、ESP/GREカウンタ、rollbackを検証する

## YAML接続点

`vpp_edges[].vpp_socket` に、対象ノードのVPP CLIまたはBinary APIの接続先を指定する。`samples/gre-vpp-data-plane.yaml` は `/run/ibuki-vpp-ns/site-a/cli.sock` と `/run/ibuki-vpp-ns/site-b/cli.sock` を例として持つ。

既存YAMLでsocketを省略した場合は、従来どおり共有root VPPを利用する互換動作とする。socket指定を含むVPP計画では、生成された `run_vpp_node` が対象ノードのsocketを選び、GRE・underlay・経路設定を対象VPPへ送る。

## 完了判定

全面移行の完了は、VPPプロセスが起動したことだけでは判定しない。次の全条件を満たした時点を完了とする。

- site-a/site-bのVPPがそれぞれのnamespace内で起動し、個別CLI/API socketへ接続できる
- 各VPPがLAN attachmentとunderlay attachmentを持つ
- Controller生成計画の全VPP命令がノード別socketへ送られる
- strongSwanが同じnamespaceのunderlayを使ってIKE/CHILD_SAを確立する
- GRE往路・復路がそれぞれのVPPで作成される
- site-a/site-bのLAN pingが暗号化状態で双方向に成功する
- 停止・再適用・rollback後も残留socket、PID、GRE、XFRM、経路がない

現段階では、VPPプロセス分離、YAML socket入力、生成計画のnode dispatch、LAN/underlay attachment生成、strongSwan側の双方向GREとIKE/CHILD_SA、暗号化データパスの双方向LAN pingを実ランナーで確認した。[strongSwan/XFRM実行 36022032962](https://github.com/nagato-home-server/ibuki/actions/runs/36022032962)では両方向とも3/3応答、損失0%、XFRM ESPの送受信シーケンス進行を確認した。[VPP Native実行 36022029474](https://github.com/nagato-home-server/ibuki/actions/runs/36022029474)でも両方向3/3応答、損失0%で、両siteのVPP ESP暗号化・復号カウンタが増加した。両backendでクリーンアップ後の再適用と、終了後のVPP socket/PID、charon PID、テストLAN interface、XFRM state/policyの残留検査が通過した。ローカルArchにVPPをビルド・導入せず、手動実行の[GitHub Actions VPP namespace smoke](../.github/workflows/vpp-namespace-smoke.yml)でUbuntu 24.04にFD.io VPPパッケージを導入して検証する。

## v2の具体的なアドレス構成

| 用途 | site-a | site-b |
| --- | --- | --- |
| IKE underlay | `203.0.113.10` | `203.0.113.9` |
| VPP underlay | `198.18.1.1/30` | `198.18.2.1/30` |
| Linux underlay peer | `198.18.1.2/30` | `198.18.2.2/30` |
| VPP LAN gateway | `10.10.1.1/24` | `10.10.2.1/24` |
| 独立client namespace | `client-a` | `client-b` |
| client LAN アドレス | `10.10.1.2/24` | `10.10.2.2/24` |
| IPsec統合時のVPP TAP | `169.254.100.1/30` | `169.254.100.1/30` |
| IPsec統合時のLinux TAP peer | `169.254.100.2/30` | `169.254.100.2/30` |
| GRE inner | `10.255.0.1/30` | `10.255.0.2/30` |
| VPP CLI socket | `/run/ibuki-vpp-ns/site-a/cli.sock` | `/run/ibuki-vpp-ns/site-b/cli.sock` |

`198.18.1.1` と `198.18.2.1` はVPPが生成するGRE outer endpointであり、Linux側のunderlay peerを経由して既存の `a-direct`/`b-direct` へ転送する。namespace実験ではIKE endpointが`203.0.113.10`／`203.0.113.9`と別なので、strongSwanはtunnel modeと固定の`/32[gre]` selectorを使用する。`dynamic[gre]` は実パケットから別の外側アドレスを選択するため、この構成では使用しない。

pingの送信元・宛先は独立したclient namespaceの`10.10.1.2`／`10.10.2.2`であり、VPP gatewayの`.1`とは区別する。clientの`eth0`からvethでVPP LAN host-interfaceへ入り、ローカルLANの戻りはVPP connected routeを使う。固定neighborを前提にせず動的ARPを試験する。AF_PACKET作成時の`cksum-gso-disable`により、今回のLinux／VPP環境でARP応答がclientへ届かない問題を回避した。Linux側のethtool設定だけでは解消しなかった。

## 起動順序

1. `vm-netns.sh setup` がsite namespaceと既存underlayを作成する
2. `vm-vpp-ns-runtime.sh start` がsiteごとのVPPを起動する
3. `SKIP_GRE=1 vm-vpp-ns-topology.sh setup` がLAN/underlay veth、LAN送信元、VPP host-interfaceを作成する
4. `eventnet_netns_plan` がYAMLを解析し、`gre-swanctl.conf` とVPP計画を生成する
5. `vm-netns-ipsec.sh gre start` が両siteのcharonを起動し、VICI経由で設定をロードする
6. `vpp-netns-route-plan.sh` が `run_vpp_node` 経由でsiteごとのVPPへGRE・経路を適用する
7. client namespaceからVPP LAN host-interfaceへ入り、GRE、Linux XFRM、underlayの順に転送する。10月9日の確認に加え、10月10日に8 vCPUで双方向各3ping・ESP・cleanupを最終確認した。性能反復ではない。ARP warm-upと測定pingは分けて保存する

## 停止順序

停止時は、CHILD_SA/charon、VPP GRE/経路、VPP host-interface、veth、VPPプロセスの順で削除する。`vm-gre-namespace-v2-smoke.sh` は終了時にこの順序を実行する。途中で失敗しても各削除は冪等に再実行できるよう `|| true` を使い、次回起動時の残留検出を妨げない。

## 実装ファイルの責務

- `include/eventnet/types.h`: `en_vpp_edge_t.vpp_socket` のデータモデル
- `src/yaml_config.c`: `vpp_socket`、`vpp_api_socket`、`vpp_cli_socket` の解析と安全な文字検査
- `examples/netns_plan.c`: `run_vpp_node` の生成とノード別VPP命令の出力
- `scripts/vm-vpp-ns-runtime.sh`: VPPプロセス、CLI/API socket、PID、ログの管理
- `scripts/vm-vpp-ns-topology.sh`: veth、host-interface、underlay、GRE、LAN routeの構成
- `scripts/vm-gre-namespace-v2-smoke.sh`: 計画生成からstrongSwan、VPP、双方向pingまでの統合入口
- `samples/gre-namespace-v2.yaml`: v2実験の入力例

実行時には `GRE_CHILD` でYAML内のCHILD名を指定する。v2サンプルでは `gre-namespace-v2`、従来サンプルでは既定値 `gre-a-b` を使う。

## VPP Native IPsec実験

Native VPP IPsecでは、GREを生成せず、現行VPPの`create ipip tunnel`と`ipsec tunnel protect`を組み合わせる。GREを必要とする経路はstrongSwan/XFRM Backendで処理し、Native BackendはL3 IPIP/IPsecトンネルとして扱う。この違いにより、nativeの疎通成功を「VPP Native GRE over IPsec」の実証とは呼ばない。

`samples/gre-namespace-v2-vpp-native.yaml` は、strongSwan/Linux XFRMを使わず、VPPのIPsec pluginでIPIP interfaceを保護する実験入力である。Tunnelへ `vpp_local_sa_id`、`vpp_remote_sa_id`、SPI、暗号鍵、認証鍵を指定すると、生成された `vpp-netns-route-plan.sh` がESP SA、`create ipip tunnel`、`ipsec tunnel protect`を出力する。Linux VMで次のように実行する。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2-vpp-native.yaml
```

Native用のVPP SA項目を含むYAMLでは、スクリプトがIntentとNative IPsecモードを自動選択する。`INTENT_ID`または`VPP_NATIVE_IPSEC=0`を指定した場合は明示設定を優先する。
Nativeサンプルの`gre_interface: ipip0`は論理的な経路interface名として使用され、実際のVPPデータプレーンはIPIPである。

[2026-09-25のGitHub Actions実行](https://github.com/nagato-home-server/ibuki/actions/runs/36022029474)で、site-a/site-bそれぞれの`esp4-encrypt-tun`と`esp4-decrypt-tun`カウンタが通信後に増加し、両方向のping、再適用、残留検査も通過した。サンプルのSAと鍵は静的な実験用であり、IKEによる鍵交換・更新や本番用の鍵管理を備えた構成ではない。

GRE自体の暗号化データパスを試す場合は、strongSwan/XFRM経路を次で実行する。

```sh
sudo sh scripts/vm-gre-namespace-v2-smoke.sh samples/gre-namespace-v2.yaml
```

Native IPIP/IPsec Backendは静的SAを使う実験用であり、strongSwan VICIからVPP Binary APIへSAを同期する本番連携は未実装である。一方、上記strongSwan/XFRM GRE BackendではIKEでSAを確立する。VPPの版によって暗号名やCLIの対応が異なるため、Native失敗時は生成計画と `show ipsec all` を記録する。鍵を含む表示は公開前に秘匿する。
