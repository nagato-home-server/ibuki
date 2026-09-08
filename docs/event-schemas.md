# Ibuki Event Schemas

IbukiのAgent、observer、`eventnetd`が交換するJSON Linesの契約です。1行が1つのJSON objectであり、空行以外は必ず既知のschemaを持つ必要があります。

## 1. Path health telemetry

Agentが定期測定したPathの品質を表します。

```json
{"schema":"ibuki.telemetry.path_health.v1","path_id":"path-direct","source":"site-a","target":"203.0.113.9","sequence":1,"rtt_ms":12.5,"packet_loss_percent":0,"jitter_ms":0,"state":"healthy","timestamp_ms":1720000000000}
```

必須フィールド:

- `schema`: `ibuki.telemetry.path_health.v1`
- `path_id`: Path識別子
- `state`: `healthy`、`degraded`、`failed`、`unhealthy`
- `rtt_ms`: 0以上の有限値
- `packet_loss_percent`: 0以上100以下の有限値
- `jitter_ms`: 0以上の有限値
- `consecutive_successes`: 連続成功回数（省略時は1または0）
- `consecutive_failures`: 連続失敗回数（省略時は1または0）
- `timestamp_ms`: 0以上のtimestamp

`source`、`target`、`sequence`はAgent出力の識別・説明用フィールドです。`source`と`target`は存在する場合に許可文字で検証され、`sequence`は存在する場合に0以上の整数として検証されます。Agentでsourceを指定しない場合は`source`フィールド自体を省略し、空文字は出力しません。ControllerのPath評価は`path_id`、状態、品質、timestampを使用します。将来の複数Agent統合では、この識別情報を測定主体・測定対象の整合性確認に利用します。

## 2. Path event

障害検出・復旧検出など、Path状態の変化を表します。

```json
{"schema":"ibuki.event.path.v1","event":"path_failed","path_id":"path-direct","timestamp_ms":1720000000000}
```

`event`は`path_failed`または`path_recovered`です。failureはPathを候補から除外し、recoveredはhealthyとして再評価します。

## 3. Tunnel event

strongSwanのCHILD SA観測結果をPath評価へ渡すための最小形式です。

```json
{"schema":"ibuki.event.tunnel.v1","event":"tunnel_state","path_id":"path-direct","tunnel_id":"tun-a-b","state":"installed","timestamp_ms":1720000000000}
```

`state`は`installed`、`rekeying`、`deleted`、`down`、`failed`、`up`を使用します。`installed`、`rekeying`、`up`はhealthy、それ以外はfailedとして扱います。

## 4. VPP route event

VPP FIB観測結果をPath評価へ渡すための最小形式です。

```json
{"schema":"ibuki.event.vpp.route.v1","event":"route_state","path_id":"path-direct","tunnel_id":"route-a-b","table_id":100,"state":"up","timestamp_ms":1720000000000}
```

`state`はTunnel eventと同じ集合を使用します。`tunnel_id`はobserverの説明用識別子として利用します。FIB出力にVRF headerがある場合は任意の`table_id`（0以上の整数）を含め、`path_id`が評価対象Pathを決めます。

Tunnel eventでは`tunnel_id`をPathのsegmentに存在するCHILD SA識別子として照合し、該当しないTunnel eventはeventnetdで拒否します。

`destination_prefix`と`next_hop`はobserverが取得したroute identityです。両方を出す場合は両方必須で、`table_id`は省略可能ですが指定時は0以上の整数でなければなりません。小数・負数・整数範囲外の値や片方だけのroute identityはControllerのtelemetry parserで拒否します。
VPP observer CLIでVRFを限定する場合は、`--event PATH_ID ROUTE_ID --table TABLE_ID`を指定します。observerは指定tableのrouteだけをevent化し、eventnetdは通常のVPP route eventと同じ入力境界で受理します。

## 5. VPP interface event

VLAN sub-interfaceまたは親interfaceの状態をPath評価へ渡す形式です。

```json
{"schema":"ibuki.event.vpp.interface.v1","path_id":"path-vpp-vlan","interface_name":"host-vpp-site-a.100","state":"up","timestamp_ms":1720000000000}
```

`state`は`up`または`down`です。`interface_name`はVPP API observerが要求したinterfaceと一致し、eventnetdはYAMLのPath端点に紐づくVPP edgeと照合します。down、存在しないinterface、別Pathのinterfaceは健康な経路として扱いません。

`show interface`に対象行が存在しない場合も、observerのevent modeは対象interfaceを`down`として出力できます。これによりsub-interface削除・停止を、eventnetdの安全側判定へ伝播できます。

VLAN IDを持つIntentでは、親interface名だけのeventは不十分です。`vpp_interface.VLAN_ID`形式のsub-interfaceが一致した場合だけupを健全状態として扱い、タグなし経路への誤フォールバックを防ぎます。

さらに、対象edgeに`allowed_vlans`が指定されている場合は、IntentのVLAN IDもその一覧に含まれていなければイベントを受理しません。設定反映時だけでなくtelemetryのidentity検証時にも同じ許可境界を適用します。

## 6. Controller status

`eventnetd --status-jsonl`が出力する判断結果です。

```json
{"schema":"ibuki.status.v1","timestamp_ms":1720000000000,"intent_id":"intent-a-b","selected_path":"path-via-hub","transition_state":"completed","reason":"active path path-direct failed; using fallback path-via-hub","excluded":[{"path_id":"path-direct","reason":"active path failed event"}]}
```

statusは入力ではなく、Controllerの判断を再現・集計するための出力です。`excluded`には候補から除外したPathと理由を含みます。文字列値はJSON escapeされます。

## 7. Validation policy

- v1で定義されていないschemaは拒否します。
- schemaなしの非空行、未完結JSON、有限値でないmetric、範囲外metricを拒否します。
- 数値はJSON numberの字句規則で検証し、`+1`、`01`、未完結の小数・指数を拒否します。
- Path、Tunnel、Route IDには許可文字の制約があります。
- 同一Pathに対するtelemetryはtimestampの古いレコードで新しいレコードを上書きしません。遅延・再送は破棄され、同一timestampは入力順を維持します。
- 同一`source`から同一Pathへ届く同一timestampのtelemetryでは、`sequence`が小さい再送を破棄します。`sequence`がないレコードは従来互換のため入力順で扱います。
- file／socketから受けるJSONLは1回の入力につき最大4MiBです。Linuxのfile入力はsymlink、非regular file、group／other書き込み可能なファイルを拒否します。
- 入力境界の確認は`sh scripts/vm-eventnet-event-smoke.sh`と`sh scripts/vm-telemetry-controller-smoke.sh`で行います。

VICIやVPP Binary APIを追加する場合も、transport固有の状態をこのschemaへ変換してからeventnetdへ渡します。Controller本体はtransportのAPI型に依存しません。
