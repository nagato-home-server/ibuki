# Namespace Runtime v2

## 目的

GRE over IPsecの実データパスでは、VPPがroot名前空間、strongSwanがサイト名前空間に分かれている構成を採用しない。各拠点のデータプレーンプロセスを同じネットワーク名前空間へ置き、GREの外側パケットとXFRM処理を同一の経路に通す。

## 新しい配置

```text
site-a namespace
  strongSwan/charon-a
  VPP-a (CLI/API socket: /run/ibuki-vpp-ns/site-a/)
  underlay interface
  LAN attachment

site-b namespace
  strongSwan/charon-b
  VPP-b (CLI/API socket: /run/ibuki-vpp-ns/site-b/)
  underlay interface
  LAN attachment
```

VPPをサイト単位に分けることで、VPPのGRE outer endpoint、Linux kernelのunderlay、strongSwanのXFRM policyが同じ名前空間で解決される。Controllerは `vpp_edges[].vpp_socket` をノード単位の接続先として選び、VPP Binary APIまたはCLIをそのソケットへ送る。

## 共通ランタイム

`scripts/vm-vpp-ns-runtime.sh` は、サイト名前空間ごとにVPPを起動・停止・状態確認する。各インスタンスには個別のCLI socket、API socket、統計用`stats.sock`、PID、ログ、API segment prefixを割り当てる。統計ソケットも分離することで、複数VPPが共有`/run/vpp/stats.sock`をbindして起動に失敗することを防ぐ。

```sh
sudo sh scripts/vm-vpp-ns-runtime.sh start
sudo sh scripts/vm-vpp-ns-runtime.sh status
sudo sh scripts/vm-vpp-ns-runtime.sh stop
```

`scripts/vm-vpp-ns-topology.sh` は各サイトnamespace内にLAN/underlay vethとVPP host-interfaceを作成し、VPPのunderlay経路と双方向GREを構成する。`scripts/vm-gre-namespace-v2-smoke.sh` はこのトポロジ、Controller生成計画、namespace内strongSwanをまとめて起動する。既存のroot VPPスクリプトは互換性確認用に残す。

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

現段階では、VPPプロセス分離、YAML socket入力、生成計画のnode dispatch、LAN/underlay attachment生成、双方向GRE構成までを実装済みとする。暗号化データパスとrollbackの実VM確認は、Linux VMで `vm-gre-namespace-v2-smoke.sh` を実行して判定する。

## v2の具体的なアドレス構成

| 用途 | site-a | site-b |
| --- | --- | --- |
| IKE underlay | `203.0.113.10` | `203.0.113.9` |
| VPP underlay | `198.18.1.1/30` | `198.18.2.1/30` |
| Linux underlay peer | `198.18.1.2/30` | `198.18.2.2/30` |
| VPP LAN attachment | `10.10.1.253/24` | `10.10.2.253/24` |
| Linux LAN peer | `10.10.1.254/24` | `10.10.2.254/24` |
| GRE inner | `10.255.0.1/30` | `10.255.0.2/30` |
| VPP CLI socket | `/run/ibuki-vpp-ns/site-a/cli.sock` | `/run/ibuki-vpp-ns/site-b/cli.sock` |

`198.18.1.1` と `198.18.2.1` はVPPが生成するGRE outer endpointであり、Linux側のunderlay peerを経由して既存の `a-direct`/`b-direct` へ転送する。namespace実験ではこれらのアドレスをVPPが所有しLinux kernel addressではないため、strongSwanは `GRE_DYNAMIC_TS=1` により `dynamic[gre]` transport selectorを使用する。固定 `/32[gre]` selectorは計画出力と従来構成で保持する。

## 起動順序

1. `vm-netns-setup.sh` がsite namespaceと既存underlayを作成する
2. `vm-vpp-ns-runtime.sh start` がsiteごとのVPPを起動する
3. `vm-vpp-ns-topology.sh setup` がLAN/underlay vethとVPP host-interfaceを作成する
4. `eventnet_netns_plan` がYAMLを解析し、`gre-swanctl.conf` とVPP計画を生成する
5. `vm-netns-ipsec-gre-start.sh` が両siteのcharonを起動し、VICI経由で設定をロードする
6. `vpp-netns-route-plan.sh` が `run_vpp_node` 経由でsiteごとのVPPへGRE・経路を適用する
7. site namespaceのLAN routeからVPP host-interfaceへ入り、GRE、XFRM、underlayの順に転送する

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
