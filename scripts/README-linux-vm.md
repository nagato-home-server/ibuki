# Linux VM Smoke Test

共有フォルダ上の `controller` を Linux VM から実行するための最小手順です。

## Script Map

まず迷ったら、次の順に使います。

| 目的 | スクリプト |
| --- | --- |
| 依存確認 | `vm-check.sh`, `vm-runtime-status.sh` |
| shell構文確認 | `vm-shell-check.sh` |
| build | `vm-build-cc.sh`, `vm-build.sh` |
| 一発デモ | `demo-mitou.sh` |
| scenario実験 | `vm-eventnet-scenario-smoke.sh` |
| netns underlay | `vm-netns-setup.sh`, `vm-netns-smoke.sh`, `vm-netns-clean.sh` |
| IPsec direct/hub/GRE | `vm-netns-ipsec.sh direct|hub <action>` / `sudo env GRE_OUT_DIR=out/gre-runtime sh scripts/vm-netns-ipsec.sh gre start` |
| VPP準備 | `vm-vpp-preflight.sh`, `vm-install-vpp-fdio.sh` |
| VPP netns | `vm-vpp-netns-*.sh`, `vm-vpp-controller-netns-smoke.sh` |
| controller netns | `vm-netns-controller-smoke.sh` |
| controller統合 | `vm-controller-integrated-runtime-smoke.sh` |

root が必要なスクリプトは `sudo sh scripts/<script>.sh` で実行します。
通常ユーザーでよいものは `sh scripts/<script>.sh` で実行します。
各shellの引数・環境変数は `docs/shell-commands.md` にまとめています。

## 0. One-shot Demo

未踏提出向けの一発デモです。通常ユーザーで実行できる範囲では、build、scenario harness、Explain JSONL、runtime plan生成まで確認します。

```sh
cd controller
sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

実IPsec/VPP runtimeも含める場合:

```sh
sudo RUN_RUNTIME=1 sh scripts/demo-mitou.sh samples/linux-vm-netns.yaml
```

## 1. Dependency Check

```sh
cd controller
sh scripts/vm-check.sh
sh scripts/vm-shell-check.sh
```

足りないものが出たら、VM 側でインストールしてください。

Debian/Ubuntu 系なら helper も使えます。

```sh
sudo sh scripts/vm-install-deps-debian.sh
sh scripts/vm-runtime-status.sh
```

## 2. Build

```sh
sh scripts/vm-build.sh
```

成果物は既定で `build-linux/` に作られます。

`cmake` がまだ無い場合は、`cc` だけで fallback build できます。

```sh
sh scripts/vm-build-cc.sh
```

この場合、成果物は `build-linux-cc/` に作られます。

## 3. Generate Apply Plan

```sh
sh scripts/vm-generate-plan.sh samples/linux-vm-netns.yaml
```

出力:

- `out/eventnet-swanctl.conf`
- `out/apply-eventnet.sh`

## 4. Namespace Underlay

```sh
sudo sh scripts/vm-netns-setup.sh
```

基本疎通:

```sh
sudo ip netns exec site-a ping -c 1 203.0.113.9
sudo ip netns exec site-a ping -c 1 203.0.113.13
sudo ip netns exec site-b ping -c 1 203.0.113.17
```

## 5. Namespace Route Smoke Test

IPsec/VPP の前に、Direct / Hub / Relay の L3 経路が namespace 内で成立するか確認します。

```sh
sudo sh scripts/vm-netns-smoke.sh
```

このテストは `10.10.1.1/24` と `10.10.2.1/24` の dummy LAN を作り、経路を direct → hub → relay に切り替えながら ping します。

片付け:

```sh
sudo sh scripts/vm-netns-clean.sh
```

## Current Runtime Shape

現在は、Linux VM 一台で次の段階まで確認できます。

- namespace underlay の direct / hub / relay L3疎通
- namespace 内の strongSwan direct IPsec
- namespace 内の strongSwan hub IPsec
- VPP host-interface による namespace間 forwarding
- controller-generated VPP route plan の実適用
- controller-generated plan による IPsec + VPP 統合 smoke

まだ「同一packetをIPsec復号後にVPPへ渡す本番gateway pipeline」ではありません。
そこは次段階で、XFRM interface と VPP interface の接続設計を詰めます。

## 6. Direct IPsec Config Generation

strongSwan daemon 起動の前段として、site-a / site-b 双方の direct tunnel 設定を生成できます。

```sh
sh scripts/vm-netns-ipsec.sh direct generate
sudo sh scripts/vm-netns-ipsec.sh direct status
```

生成先:

- `out/netns-ipsec-direct/site-a/swanctl.conf`
- `out/netns-ipsec-direct/site-b/swanctl.conf`

XFRM の掃除:

```sh
sudo sh scripts/vm-netns-ipsec.sh direct clean
```

## 7. Direct IPsec Start Attempt

`/usr/lib/ipsec/charon` がある場合、site-a / site-b namespace 内で direct tunnel を起動できます。

```sh
sudo sh scripts/vm-netns-ipsec.sh direct start
sudo sh scripts/vm-netns-ipsec.sh direct status
```

ログ:

```sh
sh scripts/vm-netns-ipsec.sh direct logs
```

暗号化された direct tunnel に実際の LAN ping が流れるか確認します。

```sh
sudo sh scripts/vm-netns-ipsec.sh direct smoke
```

この smoke test は `site-a -> site-b` の ping に加えて、`swanctl --list-sas` の ESP packet counter が増えることを確認します。

停止:

```sh
sudo sh scripts/vm-netns-ipsec.sh direct stop
```

## 8. Hub IPsec Start Attempt

direct IPsec を停止してから、`site-a -> hub-1 -> site-b` の hub 経由 route-based IPsec を起動します。

```sh
sudo sh scripts/vm-netns-ipsec.sh direct stop
sudo sh scripts/vm-netns-ipsec.sh hub start
sudo sh scripts/vm-netns-ipsec.sh hub status
```

hub path は中継ノードで複数 tunnel を扱うため、policy-based IPsec ではなく XFRM interface を使います。

- `site-a <-> hub-1`: `tun-a-hub`, `if_id 101`
- `hub-1 <-> site-b`: `tun-hub-b`, `if_id 102`

実 traffic が hub 経由の ESP に乗るか確認します。

```sh
sudo sh scripts/vm-netns-ipsec.sh hub smoke
```

ログ:

```sh
sh scripts/vm-netns-ipsec.sh hub logs
```

停止:

```sh
sudo sh scripts/vm-netns-ipsec.sh hub stop
```

## 9. Controller/YAML Selected Netns Runtime

controller が YAML intent から選んだ path を、netns 実行 wrapper に変換します。

```sh
sh scripts/vm-build-cc.sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml
cat out/netns-runtime/selected-path.txt
sh out/netns-runtime/apply-selected.sh
```

既定では `intent-a-b` の priority selection により `path-direct` が選ばれます。

fallback / hub path を明示的に検証する場合:

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml --path path-via-hub
cat out/netns-runtime/selected-path.txt
sh out/netns-runtime/apply-selected.sh
```

direct と hub の両方を controller 生成 wrapper 経由で連続確認する smoke test:

```sh
sh scripts/vm-netns-controller-smoke.sh switch samples/linux-vm-netns.yaml
```

active direct path の failure event を注入し、YAML の fallback policy で hub を選ぶ smoke test:

```sh
sh scripts/vm-netns-controller-smoke.sh fallback samples/linux-vm-netns.yaml
```

direct failure 後に hub fallback へ移り、direct recovery で priority direct へ戻る smoke test:

```sh
sh scripts/vm-netns-controller-smoke.sh recovery samples/linux-vm-netns.yaml
```

手動で見る場合:

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct
cat out/netns-runtime/selected-path.txt
sh out/netns-runtime/apply-selected.sh
```

この段階では runtime script は `path-direct`、`path-via-hub`、`path-via-relay-c`のplan生成に対応しています。Relayの実IPsec runtime適用は、複数中継Tunnelの実環境検証項目として別途扱います。

## 10. VPP Route Plan Preparation

VPP を実 interface に接続する前段として、controller-selected path から VPP route command plan を生成します。

VPP の有無を確認:

```sh
sh scripts/vm-vpp-preflight.sh
```

FD.io packagecloud repository を使う場合、まず dry-run で内容を確認します。

```sh
sudo sh scripts/vm-install-vpp-fdio.sh
```

実際に repository を追加して VPP を install する場合:

```sh
sudo DRY_RUN=0 sh scripts/vm-install-vpp-fdio.sh
sh scripts/vm-vpp-preflight.sh
```

FD.io repository の取得で `Could not resolve host: packagecloud.io` が出る場合は、VM の DNS / outbound network を確認します。

```sh
getent hosts packagecloud.io
curl -I https://packagecloud.io/
```

VPP が未導入でも、dry-run の route plan は確認できます。

```sh
sh scripts/vm-build-cc.sh
sh scripts/vm-vpp-route-plan-smoke.sh samples/linux-vm-netns.yaml
```

個別に見る場合:

```sh
sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml
cat out/netns-runtime/selected-path.txt
DRY_RUN=1 sh out/netns-runtime/vpp-route-plan.sh

sh scripts/vm-generate-netns-runtime.sh samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct
cat out/netns-runtime/selected-path.txt
DRY_RUN=1 sh out/netns-runtime/vpp-route-plan.sh
```

実 VPP に適用する場合は、VPP 起動と interface/next-hop 到達性を確認したうえで `DRY_RUN=0` を指定します。

```sh
sudo DRY_RUN=0 sh out/netns-runtime/vpp-route-plan.sh
```

現在の VPP 準備段階では route command generation までです。次の段階で VPP interface と namespace / XFRM interface の接続を詰めます。

## 11. VPP Netns Host-Interface Smoke

VPP 導入後、Linux namespace と VPP を veth + AF_PACKET host-interface で接続します。

```sh
sudo sh scripts/vm-vpp-netns-setup.sh
sudo sh scripts/vm-vpp-netns-status.sh
sudo sh scripts/vm-vpp-netns-smoke.sh
```

構成:

- `site-a:vpp-client 172.16.1.2/30` <-> `VPP host-vpp-site-a 172.16.1.1/30`
- `site-b:vpp-client 172.16.2.2/30` <-> `VPP host-vpp-site-b 172.16.2.1/30`

この VPP edge 情報は `samples/linux-vm-netns.yaml` の `vpp_edges:` にも定義します。
`eventnet_netns_plan` が生成する `vpp-netns-route-plan.sh` は、この YAML mapping の `node_id` と `next_hop` を使って VPP route を作ります。

片付け:

```sh
sudo sh scripts/vm-vpp-netns-clean.sh
```

この smoke test は IPsec とは独立して、VPP が netns 間の L3 forwarding plane として使えるかを確認します。

controller-generated VPP route plan を VPP netns 接続へ実適用する smoke test:

```sh
sudo sh scripts/vm-vpp-controller-netns-smoke.sh samples/linux-vm-netns.yaml
```

この smoke test は `eventnet_netns_plan` が生成する `out/netns-runtime/vpp-netns-route-plan.sh` を `DRY_RUN=0` で適用し、`10.10.1.0/24 <-> 10.10.2.0/24` の LAN traffic が VPP 経由で流れることを確認します。

## 12. Integrated IPsec + VPP Runtime Smoke

controller が選んだ path から、IPsec runtime と VPP netns forwarding を一つの生成 plan で連続制御します。

```sh
sudo sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
```

fallback/hub も同じ入口で確認できます。

```sh
sudo MODE=fallback sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
sudo MODE=both sh scripts/vm-controller-integrated-runtime-smoke.sh samples/linux-vm-netns.yaml
```

生成される統合 runtime:

- `out/netns-runtime/apply-integrated.sh`

この段階の統合は、同一 controller-generated plan の中で次を連続して行うものです。

- selected path の IPsec tunnel 起動
- IPsec smoke による ESP counter 確認
- VPP host-interface setup
- YAML `vpp_edges` から生成した VPP route 適用
- VPP forwarding smoke

まだ「同一packetをIPsec復号後にVPPで転送する本番gateway pipeline」ではありません。そこは次段階で、Linux/VPP interface設計とXFRM/VPP接続を詰めます。

## 13. Scenario Harness Smoke

本番 `eventnetd` の前段として、path selection / fallback / evaluated policyをCLI引数で実験できます。

```sh
sh scripts/vm-build-cc.sh
sh scripts/vm-eventnet-scenario-smoke.sh samples/linux-vm-netns.yaml
```

個別に実行する場合:

```sh
build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --active-path path-direct \
  --fail-path path-direct \
  --expect path-via-hub

build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --mode evaluated \
  --health path-direct=healthy,rtt=80,loss=0.5 \
  --health path-via-relay-c=healthy,rtt=30,loss=0.1 \
  --health path-via-hub=healthy,rtt=50,loss=0.2 \
  --compare packet_loss,latency,hop_count,path_id \
  --expect path-via-relay-c

build-linux-cc/eventnet_scenario samples/linux-vm-netns.yaml \
  --step direct-ok \
  --step direct-failed \
  --step direct-recovered \
  --step relay-best \
  --explain-json out/scenario/multistep-explain.jsonl
```

JSON出力を確認する場合:

```sh
cat out/scenario/multistep-explain.jsonl
```

AgentをUnix socketへ複数接続して一つの評価batchへ集約する場合は、Linuxで次のように起動できます。

```sh
build-linux-cc/eventnetd samples/linux-vm-netns.yaml \
  --telemetry-socket /run/ibuki/eventnetd.sock \
  --socket-accept-count 2 --socket-parallel \
  --socket-parallel-timeout-ms 5000 --state-file out/eventnetd.state
```

これは指定数の接続を同時に受信して終了する評価用モードです。無期限の本番運用は
`deploy/ibuki-eventnetd.service` を使い、まず `--apply` なしで動作と権限を確認します。
