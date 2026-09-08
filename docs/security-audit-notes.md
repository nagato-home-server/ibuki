# Security Audit Notes

Date: 2026-07-29

この文書は、PathWeaver / EventNet controller と、連携先 OSS である strongSwan / VPP を見たときの「不安な場所」を整理するためのメモです。

ここに書く内容は、すべてが確定脆弱性という意味ではありません。
ただし、未踏提出前・本番化前に優先して潰すべき攻撃面です。

## 1. 現時点の到達面

現プロトタイプが実際に触っている主な面は以下です。

- YAML config
- generated `swanctl.conf`
- generated shell script
- `swanctl`
- namespace内 `charon`
- `vppctl`
- Linux network namespace / route / xfrm interface

strongSwan / VPP の全機能を使っているわけではありません。

特に現時点では、以下は直接使っていません。

- strongSwan EAP-TTLS
- strongSwan EAP-MSCHAPv2
- strongSwan EAP-SIM/AKA
- strongSwan RADIUS
- strongSwan TLS-based EAP
- VPP IPsec plugin
- VPP IKEv2 plugin
- VPP VRRP plugin
- VPP memif

そのため、公開CVEが存在しても、現デモに直撃するものと、将来の本番化で問題になるものを分けて扱います。

## 2. strongSwan側で気になる既知CVE

ローカルの `oss/strongswan` は `6.0.7-16-g3582906332` で、NEWS上では新しめの既知CVE修正を含んでいます。

ただし、Linux VMで動かしているパッケージが `5.9.13` などの場合、ディストリビューションのbackport状況を確認する必要があります。

気になるもの:

- `CVE-2026-47895`
  - identity cloneのdouble-free。
  - unauthenticated attackerから到達し得ると修正commitに説明がある。
  - `oss/strongswan/NEWS`
- `CVE-2026-35328` - `CVE-2026-35334`
  - TLS supported_versions、PKCS#7、EAP-SIM/AKA、constraints、RADIUS、GMP RSA周辺。
  - 現プロトタイプでは多くが未使用pluginだが、本番gatewayではplugin構成次第で影響する。
- `CVE-2026-25075`
  - EAP-TTLS AVP長処理。
  - 現プロトタイプではEAP-TTLS未使用。
- `CVE-2025-62291`
  - EAP-MSCHAPv2 Failure Request処理。
  - 現プロトタイプではEAP-MSCHAPv2未使用。

VMで確認するコマンド:

```sh
strongswan version || true
swanctl --version || true
dpkg -l 'strongswan*'
apt changelog strongswan | grep -i CVE || true
```

## 3. VPP側で気になる既知/準既知領域

ローカルの `oss/vpp` は `v26.10-rc0-271-g7fe9c2669` です。

公式security advisories上で見えるもの:

- `CVE-2022-46397`
  - VPP IPsec AES-CBC IV生成。
  - 現プロトタイプではVPP IPsec pluginを使わず、strongSwan/Linux XFRMでIPsecを行っている。
- VRRP plugin heap buffer overflow advisory
  - 現プロトタイプではVRRP plugin未使用。

ただし、git log上ではCVE未付与でも安全修正に見えるものが複数あります。

- `vrrp: fix heap buffer overflow in packet trace path`
- `tcp: fix opts_len underflow on malformed data offset`
- `mss_clamp: fix missing TCP header length validations`
- `memif: fix integer overflow in descriptor validation`
- `ipsec: fix integer overflow`
- `ipsec: make pre-shared keys harder to misuse`

現プロトタイプでは `vppctl ip route add` と host-interface が中心なので、VPPの危険pluginには深く入っていません。
将来VPP IPsec / IKEv2 / memif / VRRPを使うなら、plugin単位で脆弱性調査と不要plugin無効化が必要です。

VMで確認するコマンド:

```sh
vppctl show version
vppctl show plugins
dpkg -l 'vpp*'
```

## 4. Controller側の高優先リスク

### 4.0 Observer eventのroute identity

VPP observerは`show ip fib`のprefix・next-hop・interfaceをJSONLへ変換するため、観測出力をそのまま信頼するとevent JSONの破壊や別routeの誤受理につながります。現在は許可文字を検証し、route詳細がある場合はeventnetdでPathのroute定義と照合します。strongSwan eventの`tunnel_id`もPath segmentと照合します。実VICI/VPP transport接続時は、transport側のnode identity、socket権限、期待table/interfaceの追加検証が必要です。

### 4.1 YAML由来値のcommand injection（対策状況）

不安な場所:

- `src/render_commands.c`
- `src/apply_plan.c`
- `src/command_adapters.c`
- `examples/eventnet_scenario.c`

監査時の懸念:

- YAML由来の `tunnel_id`、`path_id`、`route_destination_prefix`、`route_next_hop` は、以前shell command文字列へ入る経路があった。
- Windows互換fallbackと、明示的にshell構文を要求する任意templateには、後方互換のためshell実行経路が残っている。

特に不安な行:

現行のLinux既定経路:

- `en_apply_plan_run()`はtoken検証後に`fork`／`execvp`で実行する。
- command adapterは単純commandをshellなしで実行し、shell記号を必要とするtemplateだけ互換shell経路へ送る。
- `eventnet_scenario --generate-runtime`と`eventnet_agent`のLinux実測も引数配列実行を使う。
- `src/render_commands.c`と生成runtimeの値はallowlist検証後に出力する。

影響:

- YAMLやCLI引数を信頼しない運用にすると、root権限実行時に任意command実行へつながる可能性がある。

残る対策:

1. Windows互換fallbackと任意templateのshell経路を引数配列APIへ移行する。
2. CIDR／IP addressをallowlistだけでなく専用parserでも検証する。
3. 実VPP Binary API codec接続時も同じ入力検証と権限分離を適用する。

### 4.2 `swanctl.conf` injection（対策状況）

不安な場所:

- `src/apply_plan.c`
- `scripts/vm-netns-ipsec.sh direct generate`
- `scripts/vm-netns-ipsec.sh hub generate`

監査時の懸念:

- YAML由来値を `swanctl.conf` の設定値として出力するため、値の境界と出力権限が重要になる。

影響:

- 悪意あるYAMLを入力すると、意図しないconnectionやsecret設定を注入できる可能性がある。

現行対策と残課題:

1. Tunnel、ID、endpoint、証明書パスはYAML allowlistで検証する。
2. 生成configはLinuxで`O_NOFOLLOW`・0600のdescriptor書き込みを行う。
3. PSKの秘密管理とWindows互換書き込み経路の強化は残課題である。

### 4.3 PSKの扱い

不安な場所:

- `samples/ipsec-routes.yaml`
- `samples/linux-vm-netns.yaml`
- `scripts/vm-netns-ipsec.sh direct generate`
- `scripts/vm-netns-ipsec.sh hub generate`
- `scripts/vm-netns-ipsec.sh direct start`
- `scripts/vm-netns-ipsec.sh hub start`

根拠:

- `change-me` がデフォルトPSKになっている。
- 生成された `swanctl.conf` にPSKが平文で入る。
- start scriptで `chmod 644` している。
- load失敗時に `cat "$swanctl_conf"` でsecretをstderrへ出す。

影響:

- VM共有フォルダ・ログ・端末履歴経由でPSKが漏れる。
- demoから本番へ流用すると危険。

優先対策:

1. `PSK=change-me` を本番系では拒否する。
2. `swanctl.conf` を `chmod 600` にする。
3. `cat "$swanctl_conf"` をやめ、secretをredactした表示にする。
4. samplesには `psk: "demo-only-change-me"` と明記するか、PSK未設定で実行時必須にする。

### 4.4 YAML値のサイレント切り詰め

不安な場所:

- `src/yaml_config.c`
- `src/state.c`

根拠:

- `copy_id()` が `snprintf()` で静かに切り詰める。
- 切り詰めエラーを返さない。

影響:

- 長いIDを2つ用意すると、切り詰め後に同じIDになり、意図しないPath/Tunnel参照が起きる可能性がある。
- これは即RCEではないが、policy bypass / wrong route apply の原因になり得る。

優先対策:

1. `copy_id()` をエラー返却型にする。
2. `EN_MAX_ID_LEN` 以上の入力を拒否する。
3. YAML load後にID重複チェックを行う。

### 4.5 数値parseが緩い

不安な場所:

- `src/yaml_config.c`
- `examples/eventnet_scenario.c`

根拠:

- `atoi()` / `atof()` を使っている。
- `nan`、`inf`、負数、末尾ゴミを拒否していない。

影響:

- health / constraints評価が壊れる。
- evaluated selectionで意図しないPathを選ぶ可能性がある。

優先対策:

1. `strtod()` / `strtol()` へ変更する。
2. 末尾までparseしたか確認する。
3. 範囲を明示する。
4. RTTは `0 <= rtt <= practical_max`、lossは `0 <= loss <= 100` に制限する。

## 5. Script側の高優先リスク

### 5.1 root実行scriptの環境変数override

不安な場所:

- `CHARON="${CHARON:-/usr/lib/ipsec/charon}"`
- `VPPCTL="${VPPCTL:-vppctl}"`
- `RUN_BASE`
- `SWANCTL_WORK_BASE`
- `OUT_DIR`

根拠:

- rootで実行するscriptが、複数の実行パス・出力パスを環境変数で変更できる。
- demoには便利だが、攻撃面としては広い。

影響:

- 誤操作や悪意ある環境変数で、想定外binaryや想定外pathをrootで使う可能性がある。

優先対策:

1. `ALLOW_UNSAFE_OVERRIDES=1` がある時だけoverrideを許可する。
2. `CHARON` / `VPPCTL` は絶対pathかつroot-owned executableに限定する。
3. `RUN_BASE` / `SWANCTL_WORK_BASE` は `/run/eventnet-*` / `/etc/swanctl/eventnet-*` 配下に限定する。

### 5.2 world-readable runtime directory

不安な場所:

- `chmod 755 "$RUN_BASE" "$run_dir" "$SWANCTL_WORK_BASE" "$swanctl_work_dir"`
- `chmod 644 "$swanctl_conf"`

根拠:

- VICI socket自体はdaemon側の権限にも依存するが、周辺dir/configが広く読める。

優先対策:

1. run dirは必要最小権限にする。
2. secret入りconfは `600`。
3. status scriptでsecret入りファイルのpathや権限だけ出し、内容は出さない。

## 6. strongSwan OSS側で追加で見るなら

現時点のローカル `oss/strongswan` は既知CVE修正込みです。
新規調査として見るなら、以下が良さそうです。

- `src/libstrongswan/utils/identification.c`
  - `CVE-2026-47895` の修正箇所周辺。
  - clone / destroy / zero-length chunk。
- `src/libcharon/plugins/eap_ttls/eap_ttls_avp.c`
  - AVP length処理。
- `src/libradius/radius_message.c`
  - RADIUS attribute length処理。
- `src/libsimaka/simaka_message.c`
  - EAP-SIM/AKA attribute length処理。
- `src/libstrongswan/plugins/pkcs7/pkcs7_enveloped_data.c`
  - padding検証とNULL dereference。
- `src/libtls/tls_server.c`
  - supported_versions / ECDH public key処理。

未使用pluginでも、将来有効化するなら対象になります。

## 7. VPP OSS側で追加で見るなら

現時点のローカル `oss/vpp` は新しいですが、以下は匂いが強いです。

- `src/plugins/ikev2/ikev2.c`
  - IKE payload length、`plen - sizeof(...)`、packet parser。
  - ただし現プロトタイプでは未使用。
- `src/vnet/ipsec/esp_encrypt.c`
  - IV生成、crypto op、ESP packet length。
  - 現プロトタイプではVPP IPsec未使用。
- `src/vnet/ipsec/esp_decrypt.c`
  - ESP tail / ICV / padding / chained buffer処理。
  - 現プロトタイプではVPP IPsec未使用。
- `src/plugins/vrrp/node.c`
  - trace path overflow修正が最近ある。
  - 現プロトタイプではVRRP未使用。
- `src/plugins/memif/node.c`
  - descriptor validationのinteger overflow修正がある。
  - 現プロトタイプではmemif未使用。

## 8. まず直す順番

最優先:

1. `system()` 実行経路を潰す、または入力allowlistを入れる。
2. PSKを `change-me` から必須指定へ変える。
3. secret入り `swanctl.conf` を `600` にする。
4. secretをログへ出さない。
5. YAML値のvalidateを入れる。

次点:

1. `copy_id()` のサイレント切り詰めをエラー化する。
2. 数値parseを `strtod()` / `strtol()` に変える。
3. VM package CVE check scriptを追加する。
4. VPP plugin allowlist / disable方針をdocsへ書く。

## 9. 現時点の判断

現時点で「このcontrollerが即remote exploit可能」と断定できるものは見つけていません。

ただし、YAMLやCLI入力を信頼できないものとして扱うなら、controller側のcommand生成・残存する`system()` fallback・secret handlingは引き続き注意が必要です。Linuxの生成plan実行と既定command adapterはshellを介さない実行へ移行済みです。

未踏提出向けプロトタイプとしては、次のセキュリティ改善を入れるだけでも説明力が上がります。

- demo-only secretを明示する。
- root実行時に危険な入力を拒否する。
- generated configのsecretを守る。
- OSS CVE追跡をruntime preflightに組み込む。

## 10. 2026-07-29 対応済みメモ

`scripts/` の公開前整理で、次の quick fix を入れた。

- direct/hub IPsec config生成時、PSK未指定なら `/dev/urandom` からdemo用PSKを生成する。
- `PSK=change-me` が明示された場合は拒否する。
- secret入り `swanctl.conf` は生成先・runtime copy ともに `chmod 600` にする。
- `swanctl.conf` load失敗時にconfig本文をstderrへ出さない。
- `scripts/vm-shell-check.sh` を追加し、全shell scriptの構文確認をできるようにした。

state fileの復元もLinuxでは`O_NOFOLLOW`付きで開くようにし、サービスユーザーが意図しないsymlink先を読み込まないようにした。さらに通常ファイルであることとgroup／otherの書き込み不可を確認する。stateが存在しない場合だけ初回起動として扱い、権限エラー・symlink・危険なmodeは起動失敗として扱う。

残る主要リスク:

- 任意templateとWindows互換fallbackに残る`system()`経路の縮小。
- VMに入っている strongSwan / VPP package version のCVE確認。
- 実VPP Binary API message codec接続後の入力境界・権限分離確認。

## 11. 2026-09-08 対応済みメモ

- Tunnel、Path、Intent、Segment、RouteのYAML識別子とroute属性にallowlist検証を追加した。
- `copy_id()`の長さ超過時に黙って切り詰めず、必須項目の欠落としてYAML検証で拒否するようにした。
- status JSONLへ文字列エスケープを追加し、外部入力によるJSON破壊を防いだ。
- `en_apply_plan_run`のLinux実行をtoken検証付き`fork`/`execvp`へ変更した。Windows互換fallbackと任意templateは別経路として残し、信頼できる設定だけで使用する。
- `eventnet_scenario --generate-runtime`のLinux実行も`fork`/`exec`へ変更し、YAML／Path引数のshell解釈を排除した。実験用入力でも既定のLinux評価経路はshell文字列連結を行わない。
- `eventnet_agent`のLinux実測pingを`fork`/`execlp`へ変更し、targetをshellへ連結しないようにした。Windowsの互換経路は`_popen`を使うが、targetは許可文字検証後にのみcommandへ展開する。Windows固有のping引数と`time<1ms`出力も個別に処理する。
- command adapterのtemplateも、shellメタ文字を含まない場合はLinuxで`exec`経路へ送り、パイプやリダイレクトを明示的に使う従来templateだけをshell経路へ残す。既存templateの互換性を維持しながら、通常の単純commandのshell面を縮小した。
- 不完全telemetry、重複route、重複waypoint、required/forbidden矛盾を回帰テストで拒否する。
- state復元時に現在Intentのtraffic keyとPath所属（explicit／candidate／fallback）を照合し、別Intentや候補外Pathのstateを拒否する。複数traffic keyの復元では、既に復元したPathの状態をstandbyへ戻さない。
- YAMLの識別子・route属性・VPP edgeにallowlist検証を実装済み。入力値にshellメタ文字を許可せず、生成runtimeへの混入を拒否する。
- eventnetdのstatus JSONL出力もLinuxでは`O_NOFOLLOW`付きdescriptorで開き、symlink・非regular file・group／other書き込み可能な既存ファイルを拒否する。
- Agentの通常／追記JSONL出力もLinuxでは`O_NOFOLLOW`・regular file・group／other書き込み不可を確認して開き、新規ファイルは0600で作成する。
- eventnetdのfile telemetry入力もLinuxでは`O_NOFOLLOW`付きdescriptorで開き、regular file以外とgroup／other書き込み可能なファイルを拒否する。Agent出力とdaemon入力を同じsymlink非追従・権限境界に揃えた。
- state復元時にPathのadministrative stateも確認し、YAMLで無効化されたPathを旧stateから再びActiveにしない。
- state復元時にPathの端点と全segmentのNodeも確認し、無効化または未知のNodeを含む旧stateをActiveにしない。
- Linuxビルドでstack protector、FORTIFY、RELROを既定有効にした。これは入力検証の代替ではなく、メモリ破壊時の影響を低減するベースライン対策である。

### 常駐XFRM blockの再利用

`eventnetd --backend command --apply`で`block_non_ipsec: true`を使う場合、Controller再起動後に同じpolicyを重複追加しない必要がある。現在はapply時に`ip xfrm policy list`を読み、selector・方向・priority・`action block`が同一policyブロックに揃う場合だけ既存policyとして再利用する。送受信の片方だけが存在する場合は自動修復せず失敗させ、外部で作られた既存policyはController終了時に削除しない。

この判定はLinuxのXFRM出力形式に依存するため、実VMで`ip xfrm policy list`の出力を確認する。形式が異なる場合は、推測で緩い文字列判定へ戻さず、対象ディストリビューションの出力parserを追加する。

Tunnel削除時は、先にSA終了を実行し、終了が成功した場合にだけblock policyを削除する。SA終了またはblock削除に失敗した場合は、cleartextを許す方向へ進めない。

### command adapterの実行境界

VPP socket、VLAN、XFRM、capture用commandの組み立てでは、固定長bufferへの`snprintf`結果を検査する。入力がbuffer長以上の場合は、切り詰められたcommandを実行せず`invalid_argument`として失敗させる。POSIXの`exec`経路でもargv化前の長さを確認する。これはsocket pathやselectorが別の短い値へ化けることを防ぐための境界であり、実際のLinux VMでは長大な入力拒否も評価へ追加する。
- eventnetdのlistener／client Unix socketを`CLOEXEC`付きで作成・acceptするようにした。daemonがcommand backendをfork／execする際、Agent接続socketを子プロセスへ継承しない。
- eventnetdの初回file streamと設定reloadも共通の`en_telemetry_open_jsonl`を使うようにした。安全検査を通らない経路がreload時だけ残らないよう、telemetry file入力のopen箇所を一つへ集約した。
- eventnetdのfile telemetry入力にも4MiBの総入力上限を追加した。空行や無効行だけでrecord数上限をすり抜けてCPUを消費する入力をboundedにし、socket／parallel入力と同じ上限設計に揃えた。
