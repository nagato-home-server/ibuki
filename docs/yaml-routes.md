# YAML Route Expressions

IbukiのYAMLでは、PathがどのTunnel/Segmentで構成されるかと、そのPathを使うために投入するrouteを分けて書けます。

## Node capability

Nodeのrole、接続endpoint、利用可能なbackend能力は`nodes`で宣言できます。`administrative_state: disabled`のNodeを含むPathは選択から除外され、Intentの`required_capabilities`を満たさないPathも除外されます。能力情報はCloud VPN／FRR／VPP adapter選択へ接続する拡張境界です。

```yaml
nodes:
  - id: site-a
    role: edge
    endpoints:
      - 203.0.113.10
    capabilities:
      - ipsec
      - vpp
  - id: cloud-gateway
    role: cloud-gateway
    capabilities:
      - ipsec
      - cloud-vpn
```

`administrative_state`は`enabled`または`disabled`です。endpoint／capabilityの重複や不正文字、Node IDの重複はYAML loaderが拒否します。Node一覧を定義した場合、Tunnel・Path・Segment・route・waypointが未知のNodeを参照する設定も読み込み時に拒否します。Intentの`constraints.required_capabilities`（短縮形`capabilities`）を指定すると、Pathのsource・destinationと全Segmentの両端Nodeが指定能力を持つ場合だけ候補になります。

## GRE over IPsec

実験的な計画生成として、VPPのL3 GRE interfaceをstrongSwanのIPsecで保護する`gre_over_ipsec`を指定できます。これは論文提出までの正式Backendではなく、VPP Native IPsecとの組合せを未踏期間に検証するための境界です。Linux GREは標準Backendにしません。`local_endpoint`と`remote_endpoint`はGREの外側endpoint、`gre_local_address`と`gre_remote_address`はGRE内側のL3アドレスです。`gre_interface`はVPPで生成されるinterface名と一致させ、複数トンネルを固定する場合は`gre_instance`を指定します。`mtu`はGRE interfaceへ設定する任意のMTUです。

GRE用のstrongSwan CHILD_SA計画は、GREプロトコルをouter endpointの`/32[gre]` selectorで保護するtransport modeとして生成します。`local_endpoint`／`remote_endpoint`はstrongSwanのIKE endpoint、`gre_outer_local_endpoint`／`gre_outer_remote_endpoint`はVPP GRE outer endpointとして分離できます。統合runtimeではnamespace内のcharonを起動してVICIから設定をロードし、SA確立後にVPP GREと経路を適用します。ただし、VPPをroot namespace、XFRMをsite namespaceへ配置する現在の実験構成では、GRE復号後packetをVPPへ戻すforwarding pipelineが未完成です。詳細は`docs/gre-namespace-constraint.md`に記録しています。GREはL3カプセル化であり、GRETAP、VXLAN、EVPN、L2 bridgeによるL2延伸はこの指定に含まれません。BGP／OSPFによる動的経路交換とVPP Native IPsecは未踏期間の拡張です。

VPP Native IPsecを試す場合は、Tunnelへ `vpp_local_sa_id`、`vpp_remote_sa_id`、`vpp_local_spi`、`vpp_remote_spi`、`vpp_crypto_algorithm`、`vpp_crypto_key`、`vpp_integrity_algorithm`、`vpp_integrity_key`を追加します。`gre_interface`には`ipsec-gre0`のようなNative IPsec-GRE interface名を指定します。生成器は両方向のVPP SAと`create ipsec gre tunnel`によるinterface生成を計画します。これはstrongSwanのIKE・鍵更新とは別の静的SA Backendであり、実運用の鍵同期を意味しません。

```yaml
tunnels:
  - id: gre-a-b
    type: gre_over_ipsec
    local_node: site-a
    remote_node: site-b
    local_endpoint: 203.0.113.10
    remote_endpoint: 203.0.113.9
    gre_interface: gre0
    gre_instance: 0
    gre_outer_local_endpoint: 172.16.1.1
    gre_outer_remote_endpoint: 172.16.2.1
    gre_local_address: 10.255.0.1/30
    gre_remote_address: 10.255.0.2
    mtu: 1400
    psk: "change-me"

paths:
  - id: path-gre-a-b
    source: site-a
    destination: site-b
    route_destination_prefix: 10.10.2.0/24
    egress_tunnel_id: gre-a-b
    segments:
      - id: seg-gre-a-b
        from: site-a
        to: site-b
        tunnel_id: gre-a-b
```

## 1. 旧式の単一路由

既存互換のため、Path直下に一つだけrouteを書く形式を維持しています。

```yaml
paths:
  - id: path-legacy-single-route
    source: site-a
    destination: site-b
    route_destination_prefix: 10.10.2.0/24
    route_next_hop: 203.0.113.9
    egress_tunnel_id: tun-a-b
```

これは内部的には、`source` nodeに対する1本のrouteとして扱われます。

## 2. 論理L3 egress

route適用時のnext-hopとinterfaceは、Tunnel Backendが提供する論理L3 egressから解決します。Path／routeの処理はBackendの種類を直接判定せず、解決済みの`next_hop`と`interface_name`を共通のVPP route rendererへ渡します。

解決規則は次の通りです。

| Tunnel | next-hop | interface |
|---|---|---|
| 通常のIPsec | routeに指定した値。未指定時は`remote_endpoint` | routeに指定した値。未指定なら省略 |
| `gre_over_ipsec` | routeに指定した値。未指定時は`gre_remote_address` | routeに指定した値。未指定時は`gre_interface` |
| 将来のVTI／VPP Native IPsec | Backendが提供する内側next-hop | Backendが提供する論理interface |

routeに`interface_name`を明示した場合は、その値を優先します。Tunnel側の論理egressは不足している値だけを補完するため、VRFや複数portを指定する既存の明示routeとも併用できます。

例えば、GREの次の設定は、外側endpointではなくGRE内側のnext-hopとinterfaceをrouteへ反映します。

```yaml
tunnels:
  - id: gre-a-b
    type: gre_over_ipsec
    local_endpoint: 203.0.113.10
    remote_endpoint: 203.0.113.20
    gre_interface: gre0
    gre_local_address: 10.255.0.1/30
    gre_remote_address: 10.255.0.2

paths:
  - id: path-gre-a-b
    source: site-a
    destination: site-b
    route_destination_prefix: 10.10.2.0/24
    egress_tunnel_id: gre-a-b
```

この場合の生成結果は次のようになります。

```text
vppctl ip route add 10.10.2.0/24 via 10.255.0.2 gre0
```

`gre_over_ipsec`は現在、計画生成とroute egress解決の検証対象です。VPP GREとLinux XFRMを接続した実データパス、およびVPP Native IPsecは未踏期間の検証対象です。

## 3. 明示的な単一route

今後はこちらを基本形にします。

```yaml
paths:
  - id: path-explicit-single-route
    source: site-a
    destination: site-b
    egress_tunnel_id: tun-a-b
    routes:
      - id: dst-to-site-b
        node_id: site-a
        destination_prefix: 10.10.2.0/24
        next_hop: 203.0.113.9
```

## 3. 双方向route

片方向だけでなく、戻り方向も同じPath定義の中に書けます。

```yaml
paths:
  - id: path-bidirectional-routes
    source: site-a
    destination: site-b
    egress_tunnel_id: tun-a-b
    routes:
      - id: dst-to-site-b
        node_id: site-a
        dst_prefix: 10.10.2.0/24
        via: 203.0.113.9
      - id: return-to-site-a
        node: site-b
        prefix: 10.10.1.0/24
        via: 203.0.113.10
```

`destination_prefix`、`dst_prefix`、`prefix`、`to` は同じ意味で使えます。
`next_hop` と `via` も同じ意味です。
`node_id` と `node` も同じ意味です。

## 4. Hub/Relay向けのnode別route

Waypointを通るPathでは、source、waypoint、destinationそれぞれのrouteを明示できます。

```yaml
paths:
  - id: path-hub-node-routes
    source: site-a
    destination: site-b
    egress_tunnel_id: tun-a-hub
    waypoints:
      - hub-1
    routes:
      - id: site-a-to-site-b
        node_id: site-a
        destination_prefix: 10.10.2.0/24
        next_hop: 203.0.113.13
      - id: hub-return-to-site-a
        node_id: hub-1
        destination_prefix: 10.10.1.0/24
        next_hop: 203.0.113.14
      - id: hub-to-site-b
        node_id: hub-1
        destination_prefix: 10.10.2.0/24
        next_hop: 203.0.113.18
      - id: site-b-return-to-site-a
        node_id: site-b
        destination_prefix: 10.10.1.0/24
        next_hop: 203.0.113.17
```

この形式により、Direct、Hub、Relayを同じPath/routeモデルで扱えます。

## 5. table / metric / interface付きroute

`metric`はIbuki YAML上の抽象的な経路優先値であり、VPP CLIへ出力するときはVPPの正式な引数名である`preference`へ変換されます。

将来のVRF、policy routing、route-based IPsec連携に備え、route属性も書けます。

```yaml
paths:
  - id: path-route-attributes
    source: site-a
    destination: site-b
    egress_tunnel_id: tun-a-b
    routes:
      - id: attr-route
        node_id: site-a
        to: 10.10.2.0/24
        via: 203.0.113.9
        interface: ipsec0
        table: 100
        metric: 20
```

生成されたplanは、`table`が0より大きいrouteについて`show ip fib`でVRFの存在を確認し、未作成なら`ip table add`を実行してからrouteを投入します。`eventnetd --backend command`のexplicit route適用でも同じ確認・作成順序を使います。既存tableは再利用し、rollbackでは共有の可能性があるtableを削除しません。
`--verify-vpp`を使うcommand adapterでは、`table`／`table_id`／`vrf`で指定したtableのFIBを選択して検証します。同じprefixが別tableにも存在する場合に、別VRFのrouteを誤って合格扱いしません。

## 6. Relay直列・非対称・予備経路

複数のwaypointを持つRelay直列経路では、各中継ノードのrouteを個別に書けます。往路と復路でroute本数やnext-hopが異なる非対称経路も、同じ `routes:` 配列に並べます。

```yaml
routes:
  - id: relay-forward
    node_id: relay-c
    destination_prefix: 10.10.2.0/24
    next_hop: 203.0.113.25
  - id: relay-return
    node_id: relay-c
    destination_prefix: 10.10.1.0/24
    next_hop: 203.0.113.22
```

予備経路は通常のPathとして定義し、Intentの `candidates` に優先順で並べます。上位Pathのhealth failure時には、既存のselection/transition modelが次候補を選びます。

## 7. ユーザ変更可能な品質閾値

Path選択の品質閾値はIntentごとにYAMLで変更できます。Agentは実測値を出力し、閾値の適用と切替判断はControllerが行います。

```yaml
path_selection:
  mode: priority
  candidates:
    - path-direct-a-b
    - path-hub-a-b
  constraints:
    failure_threshold: 3
    recovery_threshold: 5
    hold_down_ms: 3000
    hysteresis_percent: 10
```

`failure_threshold`は連続失敗回数、`recovery_threshold`は連続成功回数、`hold_down_ms`は健全なActive Pathを維持する最小時間、`hysteresis_percent`は品質改善がこの割合未満の候補への不要な切替を抑制します。値は0以上で、hysteresisは0〜100の範囲です。未指定値は無効化または既定値として扱われ、YAML loaderが負値・範囲外・数値でない値を拒否します。

`eventnetd --reload-config`またはLinuxの`--reload-on-sighup`を使うと、設定ファイルを再読込できます。再読込に失敗した場合は直前の正常設定を維持します。変更後の判定はstatus／Explain出力と評価manifestへ保存し、同じtelemetryに異なる閾値を適用した比較実験に利用できます。

サンプルでは、`path-relay-chain-routes`、`path-asymmetric-routes`、`path-priority-backup` がこの3パターンに対応します。

生成例:

```sh
vppctl ip table add 100
vppctl ip route add 10.10.2.0/24 table 100 preference 20 via 203.0.113.9 ipsec0
```

## 6. 確認方法

Route表現のサンプルは `samples/route-examples.yaml` にあります。
実VPP netns runtimeで確認するためのサンプルは `samples/vpp-netns-routes.yaml` にあります。

Linux VMでは次を実行します。

```sh
cd controller
sh scripts/vm-build-cc.sh
sh scripts/vm-route-yaml-smoke.sh samples/route-examples.yaml
```

VPPが使えるLinux VMでは、明示的な双方向routeを実際のVPP forwardingへ反映して確認できます。

```sh
cd controller
sh scripts/vm-build-cc.sh
sudo sh scripts/vm-netns-setup.sh
sudo sh scripts/vm-vpp-routes-yaml-netns-smoke.sh samples/vpp-netns-routes.yaml
```

この確認では、次の形式をまとめて検証します。

- 旧式の単一路由
- 明示的な単一route
- 双方向route
- Hub/Relay向けnode別route
- table / metric / interface付きroute
- Relay直列route
- 非対称な往復route
- priority candidatesによる予備route
- VPP netns上での明示的な双方向route

不正なroute（`next_hop` 欠落）も `samples/route-invalid-examples.yaml` に置き、smoke testで拒否されることを確認します。

VPP edgeが定義されたYAMLでは、明示routeの `node_id` に対応する `vpp_edges` がない場合も拒否します。これにより、Relayのrouteを別nodeのVPPへ誤って送る設定を生成段階で止めます。

同一Nodeに複数のVPP portを置く場合は、各edgeへ一意な `port_id` を指定し、明示routeの `interface` で対象portを選びます。`port_id` が異なっていても、同じ `vpp_interface` または同じ `host_interface` を指すedgeは実runtimeで衝突するため拒否します。port未指定の従来形式でも、同一Nodeのedge重複を拒否します。

なお、`segments` がない旧式routeは、`routes` を省略した宛先routeのみ `runtime_kind: vpp` として扱います。`routes` を明示したまま `segments` を省略したPathは、適用対象のTunnel／実行Nodeを一意に決められないためruntime生成を拒否します。曖昧な設定を自動補完せず、YAMLを修正してから再生成してください。

Relay直列の全route生成は次で確認できます。

```sh
sh scripts/vm-relay-route-plan-smoke.sh samples/route-examples.yaml
```

現状の `vm-vpp-netns-setup.sh` はsite-a/site-bを1つのVPP FIBへ接続する構成です。Relayの実forwardingには、RelayごとのFIBまたはVPP instanceを分離する実行モデルが必要であり、route plan生成と分けて次段で実装します。

## 7. strongSwan証明書認証

Tunnelごとに`auth_method: pubkey`を指定すると、生成される`swanctl.conf`へ証明書認証を反映できます。`local_cert`は必須で、`remote_cacerts`は相手証明書を検証するCAファイルとして任意指定します。

```yaml
tunnels:
  - id: tun-cert-a-b
    local_node: site-a
    remote_node: site-b
    local_endpoint: 198.51.100.10
    remote_endpoint: 198.51.100.20
    local_id: site-a.example
    remote_id: site-b.example
    auth_method: pubkey
    local_cert: site-a.crt
    remote_cacerts: example-ca.crt
    local_ts: 10.10.1.0/24
    remote_ts: 10.10.2.0/24
```

秘密鍵や秘密情報はYAMLへ書かず、strongSwanの秘密情報管理（`ipsec.secrets`等）へ配置します。`auth_method`未指定時は従来どおりPSKです。生成前に証明書ファイルの存在を確認したい場合は`eventnet_yaml_demo ... --check-cert-files`を使えます。Linuxでは`--check-cert-validity SECONDS`でOpenSSLによる有効期限の事前確認もできます。Controllerは証明書の更新・失効確認までは行わないため、運用時は別の証明書管理機構と組み合わせます。

## 8. VLANごとの必須経由拠点

IntentのtrafficにVLAN IDを指定できます。VLAN 100を必ずHub経由にする場合は、`required_waypoints` と組み合わせます。

```yaml
traffic:
  source: site-a
  destination: site-b
  vlan_id: 100
path_selection:
  mode: priority
  candidates:
    - path-direct
    - path-via-hub
  constraints:
    required_waypoints:
      - security-hub
```

VLAN IDはtraffic keyにも含まれるため、同じsource/destinationでもVLANごとに選択済みPathと状態を分離できます。VPP netns runtimeでは、指定VLANの802.1Q sub-interfaceを作成・有効化し、そのinterfaceへrouteを接続するplanを生成します。`eventnetd --backend command`でも、VPP edgeが定義されていればsub-interfaceの存在を確認し、不足時だけ作成してupにします。`deny_unmatched_vlan: true`を指定した場合はVPP親interfaceへのdeny-all ACLも適用できます。`vpp_edges`の`allowed_vlans`へ一覧を指定すると、そのedgeでは一覧外のVLANを拒否します。省略時は後方互換のため全VLANを許可します。VLANごとのFIB table割当も実装済みですが、実trunk上の複数VLANとVPP間のL2分離はLinux VMでの追加評価対象です。

trunkで通過させるVLANをedgeごとに限定する場合は、次のように指定します。

```yaml
vpp_edges:
  - node_id: site-a
    vpp_interface: host-vpp-site-a
    next_hop: 172.16.101.2
    allowed_vlans:
      - 100
      - 200
```

Intentの`traffic.vlan_id`が一覧にない場合、Controllerはsub-interface作成やroute投入を行わず失敗します。これはedgeへの入力許可であり、許可されたVLAN同士の相互通信を分離するVRF/FIB policyではありません。

VLAN間を別FIBへ分離する場合は、VLANごとにIntent／Pathを分け、各Pathの全routeへ異なる`table`を指定します。例えば`samples/vlan-vrf-isolation.yaml`ではVLAN 100をtable 100、VLAN 200をtable 200へ割り当てます。これにより同じsource／destinationでも、VPP route投入先とtelemetryのtraffic keyがVLANごとに分離されます。runtime planとcommand backendはtableを先に作成し、VLAN sub-interfaceをupにした後で`set interface ip table`を適用し、最後にrouteを投入します。同じnodeのPath内で複数の明示table（table 0を含む）が混在する場合は、interfaceの所属を一意に決められないためruntime plan生成とcommand backendの両方で適用を拒否します。これはVRFのroute分離であり、実trunk上のVLAN tag付与やVPP interface間のL2分離そのものではないため、Linux VMでは各sub-interfaceとFIBを実環境で確認します。


親interface上の未タグIPv4/IPv6通信を拒否する場合は、Intent直下に次を指定します。

```yaml
    deny_unmatched_vlan: true
```

この指定は`vlan_id`を必須とし、VPP runtimeでPathに含まれるsource／destination／waypoint／segment NodeのVLAN sub-interfaceを準備した後、同じNodeの親interfaceへdeny-all ACLを適用します。生成planと`eventnetd --backend command`の両方が同じACLを設定します。ACL indexは衝突を避けるためVLAN IDとPath内edge順から決定し、単独Pathの撤去時にはinterfaceからdetachしてACLをdeleteします。別Pathが同じACLを共有する切替中はcleanupを抑制します。既定値はfalseであり、既存のVLAN設定には影響しません。`allowed_vlans`の一覧外VLANはplan生成・command backendの両方で拒否されます。VLAN間のFIB分離は`table`指定で実装済みであり、残るのは実trunk上のタグ付き通信とL2境界の実測です。

## 9. 障害・復旧の連続回数

短時間の測定揺らぎでPathが往復しないよう、`failure_threshold` と `recovery_threshold` を指定できます。`failure_threshold` は現在ActiveのPathを障害扱いにするまでの連続失敗回数、`recovery_threshold` は待機Pathを復旧候補として採用するまでの連続成功回数です。`0`は無効を表します。

```yaml
path_selection:
  mode: priority
  candidates:
    - path-direct
    - path-via-hub
  constraints:
    failure_threshold: 3
    recovery_threshold: 2
```

この制御は現在のtraffic keyに対するActive Pathを基準にするため、初回選択では通常のhealth判定を行います。閾値未到達のActive Pathは維持され、以前Activeだった待機Pathは復旧連続成功が閾値に達するまで再選択されません。未使用のfallback候補は復旧thresholdで拒否しません。telemetryが連続回数を提供しない場合は、従来どおり各レコードを1回の観測として扱います。

IPsecのtraffic selector範囲外をcleartextで通さない場合は、Intent直下に次を指定します。

```yaml
block_non_ipsec: true
```

apply planにはTunnelごとの送受信XFRM block policyとrollbackを生成します。block範囲はTunnelの`local_ts`／`remote_ts`に限定し、IKEや別の管理通信を全体denyへ巻き込みません。これはLinux XFRM反映用のplanであり、実際のカーネルpolicy適用はroot権限とstrongSwanのpolicy priorityを含めてVMで確認します。

## 10. 現時点の制約

`routes:` は複数routeの記述とplan生成には対応しています。
ただし、現時点では次の点はまだ今後の実装対象です。

- routeごとのBackend選択
- Linux `ip route` とVPP routeの完全な構文差分吸収
- routeごとのvalidation condition
- routeごとのrollback成功確認
- 複数tableを使い分ける実VPP runtime smoke（現行サンプルはtable 100の双方向routeまで）
- Policy Based Routingやflow単位route選択
## Transition retry

一時的なTunnel／forwarding backend失敗に対して、Intentの`transition`へ有限回の再試行とbackoffを指定できます。

```yaml
transition:
  strategy: graceful
  retry_count: 2
  retry_backoff_ms: 100
```

`retry_count`は追加試行回数で、`retry_backoff_ms`は試行ごとに増加する待機時間です。上限回数を超えて失敗した場合は既存のrollbackへ進みます。

## Path hold-down

健全なactive Pathからの早期切替を抑制する場合は、`path_selection.constraints`へ`hold_down_ms`を指定します。active Pathが障害状態の場合は、障害復旧を優先してhold-downを適用しません。

```yaml
path_selection:
  mode: priority
  constraints:
    hold_down_ms: 5000
```
