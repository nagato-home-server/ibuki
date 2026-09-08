# Transport Adapter Guide

この文書は、実際のstrongSwan VICIまたはVPP Binary APIをIbukiへ接続する作業者向けの実装境界を定義します。controller本体はtransport固有の型を持たず、観測値を既存のobserver型へ変換してからeventnetdへ渡します。

## 共通方針

1. transport接続を初期化する。
2. 操作callbackと観測callbackを`en_*_api_ctx_t`へ設定する。
3. 観測callbackでは外部APIの値をobserver型へ完全に変換する。
4. `*_observe_event_json()`でv1 event JSONLへ変換する。
5. eventnetdのtelemetry入力へ1行ずつ渡す。

callback未設定、対象不在、API切断、必須フィールド欠落は`EN_ERR_INVALID_ARGUMENT`または`EN_ERR_STATE_CONFLICT`として返し、健康な状態として扱わないでください。

ensure/remove callbackが成功を返しても、返却されたTunnel IDが要求IDと一致しない場合は`EN_ERR_STATE_CONFLICT`です。同様に、route観測callbackのprefixは要求prefixと一致し、table IDは未指定（`-1`）または非負でなければなりません。transport側の取り違えをcontrollerの正常状態へ流さないことが重要です。

## strongSwan VICI

libviciが導入されたLinuxでは、`-DEVENTNET_ENABLE_STRONGSWAN_VICI=ON`で`eventnet_strongswan_vici_probe`を有効化できます。Ubuntu系で`libvici.h`が`/usr/include/strongswan`に配置される場合もCMakeが自動検出します。probeは指定URI（省略時はstrongSwan既定URI）へ接続し、VICI `version` requestを1回実行してから切断します。さらに、明示したCHILD IDに限り`initiate`または`terminate`を送信できます。これはsocket権限・URI・vici plugin・control requestの疎通確認用であり、eventnetdの常駐購読を置き換えるものではありません。

接続先はstrongSwanのVICI socketです。実装対象は次のcallbackです。

```c
en_error_code_t (*ensure_tunnel)(void *, const en_tunnel_t *, en_tunnel_t *);
en_error_code_t (*remove_tunnel)(void *, const en_tunnel_t *, en_tunnel_t *);
en_error_code_t (*observe_tunnel)(void *, const char *, en_strongswan_sa_observation_t *);
```

`observe_tunnel`は指定されたCHILD IDのSAを取得し、`state`を`EN_TUNNEL_ESTABLISHED`、`EN_TUNNEL_REKEYING`、`EN_TUNNEL_FAILED`などへ変換します。健康な確立状態は`EN_HEALTH_HEALTHY`、削除・失敗・切断は`EN_HEALTH_FAILED`にします。

libvici有効ビルドでは、`en_strongswan_vici_client_open()`で得たclientを`en_strongswan_vici_bind_client()`へ渡すことで、この3 callbackを実VICIへ接続できます。`ensure_tunnel`は`initiate`後にCHILD SAを再観測し、`remove_tunnel`は`terminate`を実行します。VICI socketが使えない環境ではbindを成功扱いにせず、既存のcommand adapterへ切り替えてください。

controllerまで含む接続確認には、`eventnet_strongswan_vici_controller_probe VICI_URI YAML [INTENT_ID]`を使います。これはYAMLを読み、VICI adapterをcontrollerへ渡してIntentを1回reconcileします。VPPはこのprobeではmockのため、VICI接合とcontroller状態遷移の確認用です。VPP実transportは別途VPP APIまたはcommand adapterで検証します。

```c
char event[512];
en_strongswan_vici_observe_event_json(
    &vici_context, child_id, path_id, timestamp_ms, event, sizeof(event));
```

生成されたeventは`ibuki.event.tunnel.v1`です。VICI event購読では、up、down、rekey、delete、DPD failureを同じ変換関数へ集約してください。

`observe`はVICIの`list-sa` eventをcallbackで受け、対象CHILDの`state`を既存のobserverへ渡します。`monitor`は`child-updown` eventを有限時間購読し、socket切断をエラーとして上位の再接続runnerへ返します。monitorの標準出力は`tunnel.v1` JSONLだけ、完了・再接続ログは標準エラーへ出すため、`monitor | eventnetd --telemetry-stdin`の形で接続できます。`monitor-forever CHILD_ID PATH_ID 0`はsocketを切断まで保持し、無期限のevent配信に使えます。対象不在・state欠落・切断はhealthyにせず、service managerからSIGTERM／SIGINTで停止します。

## VPP Binary API

接続対象はVPPのroute/FIBとinterface状態です。実装対象は次のcallbackです。VLAN付きPathでは、sub-interface作成後に対応FIBへ割り当てるcallbackも必要です。

```c
en_error_code_t (*install_path)(void *, const char *, const en_path_t *);
en_error_code_t (*remove_path)(void *, const char *, const en_path_t *);
const char *(*active_path)(void *, const char *);
en_error_code_t (*observe_route)(void *, const char *, en_vpp_route_observation_t *);
en_error_code_t (*observe_interface)(void *, const char *, en_vpp_interface_observation_t *);
```

操作callbackを注入する`en_vpp_api_message_ops_t`には、routeの追加・削除、VLAN sub-interfaceの作成・削除、VRF tableの準備に加えて、`set_vlan_interface_table(void *, const en_vpp_edge_t *, int vlan_id, int table_id)`があります。VLAN付きPathでrouteに明示tableがある場合、このcallbackを省略するとAPI adapterは適用を拒否します。Pathのwaypointに対応するedgeもsub-interface作成・削除の対象です。SDKの生成messageはこのcallback内で実装し、Controller本体へVPP SDKの型を漏らしません。

`observe_route`は指定prefixのFIBを取得し、`present`、`next_hop`、`interface_name`を設定します。route不在、API timeout、interface downは正常経路として返さないでください。

`present=true`の場合は`next_hop`を必須とし、next-hopとinterfaceにはcontrollerの許可文字（英数字、`:`、`/`、`.`、`_`、`-`）以外を含めません。Binary APIから得た値をそのままcommandやeventへ流さず、adapter境界で検証します。

```c
char event[512];
en_vpp_api_observe_event_json(
    &vpp_context, prefix, path_id, route_id, timestamp_ms, event, sizeof(event));
```

生成されたeventは`ibuki.event.vpp.route.v1`です。VLAN Policyでは、親interface、VLAN sub-interface、routeの観測を同一Pathへ集約し、sub-interfaceとrouteの両方が成立した場合だけhealthyとして扱います。どちらかが未観測またはdownならfallback対象です。

VLAN Policyでは`en_vpp_api_observe_interface()`で親interfaceまたはVLAN sub-interfaceの存在・up状態を取得します。callbackの返却interface名が要求名と異なる場合は、route観測と同様に`EN_ERR_STATE_CONFLICT`とします。

ただしVLAN付きIntentのeventnetd入力では親interfaceを代用せず、IntentのVLAN IDに対応するsub-interfaceを要求します。

## ビルドと確認

依存の有無はLinux VMで確認します。

```sh
sh scripts/vm-vpp-api-preflight.sh
sh scripts/vm-charon-config-probe.sh
sh scripts/vm-build-cc.sh
```

VPP SDKが存在する場合だけ、VAPI生成コードを用いた実装を`src/`へ追加し、`EVENTNET_ENABLE_VPP_API=ON`でビルドします。CMakeは`libvapi`、`libvapiclient`、`libvppapiclient`と、plugin配下の`vapi/vapi.h`を候補として検出します。標準外prefixへSDKを配置した場合は`VPP_PREFIX=/opt/vpp`を指定します。依存がない環境では、既存のvppctl adapterとVICI socket指定付きswanctl adapterを壊さずに維持します。

## 受け入れ条件

- 既存のCMakeビルドとCTestが成功する。
- observer callbackの正常系・未設定系・API失敗系をテストする。
- 生成eventを`en_telemetry_parse_json_line()`が受理する。
- eventnetdへ渡したeventでfallback/recoveryが再現できる。
- secret、socket path、APIエラーの詳細を不要にログへ出さない。
- 実transport未導入時も既存の論文評価スクリプトが実行できる。
