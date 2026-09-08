# VPP Binary API Implementation

この文書は、既存の`vppctl` backendを壊さずにVPP Binary API transportを追加するための作業手順です。現在の実装は`en_vpp_api_ctx_t`と`en_vpp_api_adapter()`でcallback境界までを提供し、VPP SDK固有のmessage生成はこの境界の外側に置きます。

## 現在の境界

- `install_path`: 選択済みPathのroute／sub-interfaceをVPPへ反映する。
- `remove_path`: Path切替後の旧route／sub-interfaceを撤去する。
- `active_path`: traffic keyごとの適用Pathを返す。
- `observe_route`: destination prefix、next-hop、VRF tableを観測する。
- `observe_interface`: VLAN sub-interfaceの存在とup状態を観測する。

実装は`include/eventnet/vpp_api_adapter.h`、`src/vpp_api_adapter.c`にあり、controller本体はVPPのmessage型を参照しません。`include/eventnet/vpp_api_transport.h`、`src/vpp_api_transport.c`には、SDK有効時の`vapi_ctx_alloc`→`vapi_connect`→受信FD取得／generic event callback／dispatch→disconnect/freeという接続ライフサイクルを追加しています。SDK無効時は接続を成功扱いせず、既存の`vppctl` backendへ戻せます。

## SDK確認

```sh
sh scripts/vm-vpp-api-preflight.sh
VPP_PREFIX=/opt/vpp sh scripts/vm-vpp-api-preflight.sh
```

依存が揃った場合のみ、次で有効化します。

```sh
EVENTNET_ENABLE_VPP_API=ON sh scripts/vm-build.sh
VPP_PREFIX=/opt/vpp EVENTNET_ENABLE_VPP_API=ON sh scripts/vm-build.sh
```

有効ビルド後は、routeを変更しない接続確認を次で行えます。

```sh
build-linux-cc/eventnet_vpp_api_transport_probe
```

このprobeはVPP APIへ接続し、受信FDを取得して切断します。route／VLAN／VRFの変更は行わないため、SDKとVPP API endpointの接続確認に限定して使います。

`vapi/vapi.h`の配置、生成APIのversion、接続endpoint、route／interface message名は、導入したVPP SDKのヘッダを一次情報として確認します。VPP versionを推測してmessage名を固定しません。ライブラリ名は配布形態により`libvapi`、`libvapiclient`、`libvppapiclient`があり得ます。`vm-vpp-api-discover.sh`はこれらに加えてdpkg、`pkg-config`、dynamic linkerの情報も保存します。

`vapi.h`は接続・送受信の低レベルtransportを提供し、route等の高レベルmessage形式はSDKごとに生成されるAPIを使う構造です。そのため、本プロジェクトでは低レベルtransportを共通C層として固定し、`ip_route_add_del`等の生成messageは導入SDKのversion確認後に専用adapterへ実装します。

VPP API transportの公開ヘッダは`vapi/vapi.h`の型を使用するため、CMakeではSDK include pathとlibraryをconsumerへ伝播させます。これにより`EVENTNET_ENABLE_VPP_API=ON`時も、controller libraryをリンクする実行ファイルが同じSDK型を解決できます。

## 実装順

1. SDKの接続初期化、受信FD取得、generic event callback、dispatch、切断を専用transport型へ閉じ込める。
2. route add／deleteを`install_path`／`remove_path`へ接続する。
3. VRF table、VLAN sub-interface、interface stateを必要なmessageで接続する。
4. route／interface観測を既存observer型へ変換する。
5. 失敗時の戻り値、timeout、再接続、idempotentな再適用を実装する。
6. VPP実機でdirect、fallback、VLAN、rollbackを検証する。

## 受け入れ条件

- SDK未導入時は従来の`vppctl` backendがビルド・実行できる。
- Binary API有効時もcontrollerのPath selectionとTransition型が変わらない。
- routeのprefix、next-hop、table、interface identityを検証してからhealthyと判定する。
- VPP切断、message失敗、対象interface downを成功扱いしない。
- 同じPathを再適用してもrouteやsub-interfaceを重複作成しない。
- route／VLAN messageの途中失敗時は、その呼び出しで成功した操作だけを逆順で撤去し、未実行の対象を削除しない。
- `vm-evaluate.sh vpp-api`と実VPP runtime smokeのログへ、SDK検出結果と未実装範囲を保存する。

現時点では、SDK検出、接続ライフサイクル、受信FD、generic callback境界までが完了しています。route／VLAN／VRFの生成API message送信と、VPPイベント購読の具体的なmessage transportは未実装です。生成コードの版差を吸収する専用adapterをこのtransport上へ追加する段階です。

低レベルtransportには、SDK生成型messageを接合する`en_vpp_api_transport_alloc_message`／`send_message`／`free_message`を追加しました。message ID・payloadの構築はVPP SDK生成コード側へ委ね、Ibuki側は接続、所有権、送信結果だけを管理します。次段階ではVPPの対象リリースで生成headerを固定し、`ip_route_add_del`、VLAN sub-interface、VRF tableのcodecをこの境界へ実装します。

生成コードを接続するための`en_vpp_api_apply_path_operations`と`en_vpp_api_message_adapter`を追加しました。明示routeではVRF準備後にVLAN sub-interfaceを対応FIBへ割り当て、その後にrouteを追加します。同一VRF tableの準備callbackとVLAN interface table callbackは1回に抑え、`allowed_vlans`にないVLANや同一nodeのtable競合はAPI経路でも拒否します。Pathのsource／destinationだけでなく`waypoints`に対応するedgeもVLAN操作対象へ含めます。削除時はrouteを先に撤去してからsub-interfaceを削除するため、既存routeが残ったままinterfaceを消す順序を避けます。`en_vpp_api_message_adapter`はこの操作列をControllerの`en_vpp_adapter_t`へ接続します。実際のVPP message生成は`en_vpp_api_message_ops_t`へSDK版ごとのcallbackを注入します。
VLAN付きPathで明示tableを使う場合に`set_vlan_interface_table`が未設定なら、適用開始前に拒否します。これにより、VLAN routeだけが投入されてinterfaceがdefault FIBへ残る部分適用を防ぎます。

操作途中でadd routeまたはVRF準備が失敗した場合は、すでに成功したrouteを逆順で削除し、作成済みVLAN sub-interfaceも撤去します。VRF table自体は他のPathと共有される可能性があるため、自動削除しません。なお、実VPPでのrollback成功確認は生成message codec接続後の受入試験に残ります。
install操作ではrollbackに必要なroute removeとVLAN remove callbackも事前に要求します。未設定のまま部分適用を開始しないため、失敗時のNULL callback呼び出しを防ぎます。
生成messageの送信前には`en_vpp_api_transport_is_message_available`で対象VPPがそのIDを提供するか確認し、API version差による不正送信を避けます。
