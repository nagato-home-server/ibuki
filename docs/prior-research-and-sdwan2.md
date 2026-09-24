# Ibukiの先行研究上の位置付けとONUG SD-WAN 2.0への対応状況

更新日: 2026年9月19日

## 1. 本文書の目的

本書は、Ibukiに関係する先行研究、既存OSS、一般化された制御方式を整理し、Ibukiが既存技術と何を共有し、どの部分を研究対象としているかを明確にするものである。あわせて、ONUGが示したSD-WAN 2.0の方向性に対して、Ibukiの設計思想、現在の実装、将来計画がどこまで対応しているかを区別して示す。

Ibukiを「従来に存在しなかったSD-WANそのもの」として位置付けるのは正確ではない。Intentによる要求記述、監視値に基づく経路選択、ControllerとAgentの分離、IPsecによる拠点間接続、障害時の経路切替は、既存研究や商用・OSSのSD-WANですでに扱われている。Ibukiの中心的な研究課題は、これらの機能の存在ではなく、通信経路を状態を持つ`Path`として明示し、経路の選択と切替を分離したうえで、準備、検証、転送変更、切替後検証、安定化、Rollback、Fallbackまでを観測可能な状態遷移として扱う点にある。

## 2. Ibukiが対象とする問題

一般的な経路制御では、ある時点でどの経路を選ぶかが主な問題になる。しかし実際の拠点間通信では、新しいTunnelが利用可能になる前に旧経路を削除すると通信断が発生し、新経路へ転送を変更した後に疎通できなければ復旧操作が必要になる。また、strongSwan、VPP、既存VPNルータ、将来のCloud VPNでは、事前確立、状態観測、経路更新、Rollbackなどの能力が異なる。

そこでIbukiでは、次の問題を一つの制御モデルで扱う。

- セキュリティ制約、管理者ポリシー、Health、性能条件から利用可能なPathを選ぶ。
- Tunnelの確立と実際の転送経路の変更を別の操作として扱う。
- `Preparing`、`Validating`、`Commit Applied`、`Post Validation`、`Stable`などの状態によって切替途中を表現する。
- 切替失敗時に直前の`Stable Path`へ戻すRollbackと、安全な別経路へ退避するFallbackを区別する。
- Backendごとの差をAdapterとCapabilityとして公開し、実行できない操作をControllerが判断できるようにする。
- 選択候補、除外理由、観測値、状態遷移、失敗理由をExplainログへ記録する。

このためIbukiは、経路選択アルゴリズムだけを提案する研究でも、特定のVPN製品を作る研究でもない。複数のTunnel・Forwarding実装に対して、Path選択とPath遷移を比較、再現、評価するための研究基盤として位置付ける。

## 3. 先行研究・関連システムとの比較

### 3.1 ONOS Intent Framework

ONOSのIntent Frameworkは、利用者が個々のFlow Ruleを直接記述するのではなく、接続性やポリシーをIntentとして宣言し、それをネットワーク構成へ変換する仕組みを提供する。この考え方は、IbukiがYAMLで通信要求とPath選択条件を記述するうえでの重要な先行概念である。

IbukiもIntent-based Networkingの考え方を利用するため、「Intentを使うこと」自体は新規性ではない。ONOSが汎用的なSDN制御とIntentのコンパイルを主対象とするのに対し、IbukiはIPsecを中心とする拠点間通信へ対象を絞り、選択されたPathをどの手順で安全に有効化し、失敗時にどこへ戻すかを状態遷移として扱う。したがって差分は、Intentの有無ではなく、Tunnel、Forwarding、Health、Path Transition、Rollbackを一つの観測可能なモデルに結び付ける点にある。

参考: [ONOS Intent Framework](https://javadoc.io/static/org.onosproject/onos-api/1.13.5/org/onosproject/net/intent/package-summary.html)

### 3.2 Kubernetes Controller/Reconciliation

KubernetesのControllerは、宣言されたDesired StateとObserved Stateを比較し、現在状態を目標状態へ近づけるReconciliationを継続する。IbukiのDesired State、Observed State、Active Stateの分離や、Controllerが障害・Health変化を受けて再評価する構造は、この一般化されたController設計と共通する。

したがって、「Desired Stateと現在状態を比較すること」や「Controller loopを持つこと」はIbuki固有の新規性ではない。一方、ネットワークPathの変更には通信断、非対称経路、Tunnel確立待ち、複数Agentへの反映順序など、一般的なリソース更新とは異なる影響がある。IbukiはReconciliationの考え方を通信経路へ適用し、`Commit Applied`と疎通確認後の`Stable`を分け、時間的な切替過程と通信影響を評価対象にする。

参考: [Kubernetes Controllers](https://kubernetes.io/docs/concepts/architecture/controller/)

### 3.3 flexiWAN

flexiWANは、中央管理、Edge、Agent、VPP、FRRなどを組み合わせたオープンソースのSD-WAN実装であり、実ネットワークの構築・運用に必要な管理機能を広く提供する。Controller-Agent構成、VPPの利用、経路制御、監視、装置管理など、Ibukiと共通する構成要素は多い。

そのため、Ibukiの差分を「OSSでSD-WANを構築したこと」や「VPPを利用したこと」と説明することはできない。flexiWANは実用的なSD-WAN製品・基盤として機能を統合することを主眼とする。これに対してIbukiは、Path選択とPath切替を研究上独立した対象として扱い、Backendの能力差、切替途中の状態、Rollback/Fallback、判断理由を比較可能にすることを主眼とする。IbukiはflexiWANの代替製品を目指すのではなく、SD-WAN内部のPath制御を分解して研究できる基盤を目指す。

参考: [flexiWAN Documentation](https://docs.flexiwan.com/)、[flexiWAN Feature Description](https://docs.flexiwan.com/overview/feature-desc.html)

### 3.4 Maralitの修士論文とTroiaらのOSS SD-WAN

Alvin Jan Maralitの修士論文 *An enterprise network based on SD-WAN* は、OSSを用いた企業ネットワーク向けSD-WANを扱っている。その後のTroia、Zorello、Maralit、Maierによる *SD-WAN: An Open-Source Implementation for Enterprise Networking Services* は、OpenDaylight、Open vSwitch、監視機構、ポリシーに基づくPath Selectionを組み合わせた実装を示している。監視した遅延や損失がQoS条件を外れた場合に、サービス要件を満たすTunnelを選び、Open vSwitchのFlow Tableを変更する構成である。

この研究系譜はIbukiに非常に近く、OSS、Telemetry、ポリシー、複数Tunnel、動的な経路切替という大部分の基本要素をすでに含む。したがって、Ibukiの差分を単に「監視値から最適経路を選択する」と説明しても明確な新規性にはならない。

両者の主な違いは、TroiaらがQoS条件に応じたサービス別Path SelectionとFlow Ruleの変更効果を中心に評価するのに対し、Ibukiは切替操作そのものを複数段階へ分解する点にある。Ibukiでは、選択されたTunnelへの転送変更だけでなく、事前準備、利用可能性検証、Commit、切替後のEnd-to-End検証、Stable判定、失敗時のRollback/Fallbackを明示する。また、Open vSwitchを前提とせず、strongSwan、VPP、既存IPsec装置、将来のCloud VPNをAdapter/Capability境界から扱うことを目標とする。

なお、Maralitの修士論文とTroiaらの論文は著者と研究内容が連続しているため、別々の独立した方式として数を増やすのではなく、一つの研究系譜として比較するのが妥当である。

参考: [Maralit, *An enterprise network based on SD-WAN*](https://www.politesi.polimi.it/handle/10589/139954)、[Troia et al., DOI: 10.1109/ICTON51198.2020.9203058](https://doi.org/10.1109/ICTON51198.2020.9203058)

### 3.5 ASANO System／キャンパスSD-WAN

中村、関谷、工藤による「オープンソースソフトウェアを用いたキャンパスSD-WANの検討」と、その成果であるASANO Systemは、遠隔拠点やキャンパス間でVLANを安全かつ柔軟に延伸するOSSベースのSD-WANを扱っている。ASANO System Version 1.0はIPsecに加えてVXLAN、EVPN、DMVPN、NHRPなどを組み合わせ、マルチポイントのオーバーレイとL2延伸を実現する。これは国内における実運用志向の重要な先行事例である。

IbukiはASANO SystemのL2延伸機能を再実装しない。標準対象を拠点間IP PrefixのL3 Path制御とし、どのPathを選ぶか、どの順序で切り替えるか、失敗時にどう復旧するかに対象を限定する。また、ASANO Systemがキャンパスネットワークの構築・運用とVLAN延伸を中心とするのに対し、IbukiはTunnel方式やForwarding方式が異なっても共通に扱えるPath Transition ModelとExplain機構を研究対象とする。

したがって、ASANO Systemに対するIbukiの差分は「OSS」「IPsec」「中央管理」ではない。L2オーバーレイの提供を目的とするASANO Systemに対し、IbukiはL3 Pathの選択・遷移・失敗回復を分解し、複数Backend間で比較可能にする点にある。

参考: [ASANO System公式サイト](https://asano.nc.u-tokyo.ac.jp/)、[東京大学情報基盤センター年報に記載された研究成果](https://www.itc.u-tokyo.ac.jp/Annual_Report/no19/AnnualReportNo19.pdf)

### 3.6 Google B4、Microsoft SWANなどのWAN Traffic Engineering

Google B4やMicrosoft SWANは、大規模WANにおいて集中制御とTraffic Engineeringを用い、複数経路の帯域利用率や優先度を最適化する代表的研究である。これらは、WAN全体をソフトウェアで制御し、需要やネットワーク状態に応じて経路・帯域を変更できることを示した。

Ibukiも観測値とポリシーからPathを選択する点では共通するが、対象規模と研究目的が異なる。B4やSWANは大規模なプライベートWANの帯域最適化を中心とするのに対し、Ibukiは比較的小規模な拠点間VPNを含む異種環境を対象とし、セキュリティ制約、既存IPsec装置、Tunnel確立、切替手順、Rollback、説明可能性を扱う。IbukiはTraffic Engineeringの最適化性能でこれらを上回ることを目的としない。

参考: [Google B4](https://research.google/pubs/b4-experience-with-a-globally-deployed-software-defined-wan/)、[Microsoft SWAN](https://www.microsoft.com/en-us/research/publication/achieving-high-utilization-with-software-driven-wan/)

### 3.7 Path-Aware Networking、Service Function Chaining、Segment Routing Policy

IETFのPath-Aware Networking、Service Function Chaining、Segment Routing Policyは、通信経路や経由機能を明示的なポリシー対象として扱う点でIbukiと関係する。特に、特定のセキュリティ拠点を必ず経由させるWaypoint制約は、Service Function ChainingやSegment Routing Policyと問題意識を共有する。

ただしIbukiは、新しいPacket HeaderやInternet Architectureを提案するものではない。既存のIPsec TunnelとL3 Forwardingを利用し、PathをController上の制御リソースとして表現する。VLANや通信条件に対して`required_waypoints`を指定し、条件を満たさないPathを候補から除外する設計は、既存データプレーン上で管理者のセキュリティ方針を実現するためのものである。

参考: [RFC 9217: Current Open Questions in Path-Aware Networking](https://www.rfc-editor.org/rfc/rfc9217.html)、[RFC 7665: Service Function Chaining Architecture](https://www.rfc-editor.org/rfc/rfc7665.html)、[RFC 9256: Segment Routing Policy Architecture](https://www.rfc-editor.org/rfc/rfc9256.html)

### 3.8 国内の隣接研究

江戸、和泉、阿部、菅沼による「災害リスクを考慮したネットワークの経路制御手法の提案と評価」は、NodeとLinkの災害リスクおよび変化するネットワーク状態から、SDNを用いて動的に経路を制御する。通常時の遅延やLossだけでなく、管理上重要なリスクを経路選択へ取り込む点は、Ibukiが性能条件よりSecurity ConstraintやAdministrative Policyを優先し、特定地域・拠点・Waypointを制約として扱う考え方に近い。

ただし同研究の中心は、災害時のデータ転送を有効にするRisk-aware Routingである。Ibukiは災害リスクの推定手法を提案するのではなく、外部で評価されたリスクや管理者の制約をPath Selectionへ入力し、選択後のTunnel準備、転送変更、End-to-End検証、Rollback/Fallbackまでを扱う。このため、リスクをどのように算出するかと、その判断に基づく経路をどのように安全に適用するかという役割の違いがある。

木村による「地域医療情報交換ネットワークのためのVPN設計について」は、動的IP、NAPT、Address衝突など、異なる通信環境の制約を考慮したIPsec VPNの構成方法を扱う。Ibukiが既存Routerや閉塞的な拠点を標準IPsecでHubへ参加させる構想に対して、現実のネットワーク制約を吸収するVPN設計が以前から重要課題であったことを示す先行事例である。

一方、同研究は地域医療Networkを成立させるVPN接続方式が中心であり、複数Pathの継続的選択や状態遷移モデルを主題とはしない。Ibukiでは、個々のNAT TraversalやAddress設計を置き換えるのではなく、それらによって成立したTunnelをCapabilityの異なるPathとしてControllerから選択・遷移させる。

参考: [江戸ほか「災害リスクを考慮したネットワークの経路制御手法の提案と評価」](https://www.jstage.jst.go.jp/article/ieejeiss/137/3/137_532/_article/-char/ja)、[木村「地域医療情報交換ネットワークのためのVPN設計について」](https://www.jstage.jst.go.jp/article/jami/22/6/22_465/_article/-char/ja)

## 4. 先行研究に対するIbukiの位置付け

先行研究との関係をまとめると、Ibukiの構成要素の多くは既存技術から説明できる。IntentはONOS、状態収束はKubernetes、OSS SD-WANの統合はflexiWAN、Telemetryに基づくPath SelectionはMaralitおよびTroiaら、キャンパスでのOSSオーバーレイ運用はASANO System、大規模WANの集中制御はB4やSWAN、Waypointによる経由制約はService Function Chaining、災害リスクや異種VPN環境を考慮する設計は国内の隣接研究に対応する先行概念がある。

Ibukiの研究上の位置付けは、それらを初めて発明したというものではない。既存研究で個別に扱われてきたIntent、Tunnel、Forwarding、Health、Path Selection、状態収束、障害回復を、通信Pathの切替過程を評価するための一つのモデルへ整理した点にある。特に、次の組合せを研究対象として明示する。

1. Path SelectionとPath Transitionを分離する。
2. 転送変更済みの`Commit Applied`と、End-to-End検証済みの`Stable`を分離する。
3. 最後のStable Pathへ戻すRollbackと、安全経路へ退避するFallbackを分離する。
4. Tunnel操作とForwarding操作をAdapterとして分け、Capabilityによって実装差を公開する。
5. 判断入力、候補、除外理由、遷移、結果をExplainログとして保存する。
6. 同じモデル上でImmediate、Graceful、Flow Preserveなどの切替方式を比較できるようにする。

一文で表すなら、Ibukiは「SD-WANを初めて実装する研究」ではなく、**SD-WAN内部のPath切替を、状態、能力、失敗回復、説明可能性の観点から分解して研究可能にする基盤**である。

## 5. ONUGが示すSD-WAN 2.0

ONUGのSD-WAN 2.0 Working Groupは、SD-WANを拠点間Overlayの構築だけに限定せず、Hybrid Multi-Cloud環境へ接続とポリシーを広げる方向性を示している。代表的な対象には、拠点からSaaS/IaaSへの直接接続、複数Cloud ProviderのSD-WAN Fabricへの接続、拠点とCloudのSecurity、SD-WAN ControllerとCloud APIの統合、End User向けSD-WAN Clientが含まれる。また、Application Performance、Scale、Security Policy、On-PremisesとCloud Securityの統合、複数管理DomainにまたがるOrchestrationも課題として挙げられている。

さらにONUGの議論では、異なるSD-WAN間の相互運用に向けて、標準的なIPsec、BGP、上位Orchestration、Vendor APIによるTelemetry・Policy連携などが検討されている。したがってSD-WAN 2.0は、単一製品の機能表というよりも、複数の拠点、回線、Cloud、Security Service、管理Domainを共通Policyの下で連携させる設計方向と理解するのが適切である。

参考: [ONUG SD-WAN 2.0 Working Group](https://onug.net/project-teams/working-group-template/)、[ONUG Working Groups Are Building Hybrid Multi-Cloud Reference Solutions](https://onug.net/blog/onug-working-groups-are-building-hybrid-multi-cloud-reference-solutions/)、[ONUG Working Groups: The Philosophy for 2019 is “Less is More”](https://onug.net/blog/onug-working-groups-the-philosophy-for-2019-is-less-is-more/)

## 6. IbukiのSD-WAN 2.0への対応状況

### 6.1 設計思想として適合する部分

Ibukiは、複数PathをPolicyとHealthから選択し、IPsec OverlayとL3 Forwardingを中央のControllerから扱う。このため、複数WAN経路の利用、動的なTraffic Steering、可用性に基づくFallbackというSD-WANの基本思想に適合する。

また、特定VLANや通信種別に対して必須Waypointを指定する設計は、Security Policyと経路制御を結び付けるものである。例えば「VLAN 100の通信はSecurity Hubを必ず経由する」という制約をPath選択条件とし、そのWaypointを含まないDirect Pathを候補から除外できる。この考え方は、ONUGが重視するBranch/Cloud SecurityとPolicyに基づく接続制御に整合する。

Adapter/Capability ModelもSD-WAN 2.0との親和性が高い。strongSwan、VPP、既存IPsecルータ、Cloud VPNなどを同一実装と見なすのではなく、`can_pre_establish`、`can_observe_state`、`can_atomic_forwarding_update`などの能力差をControllerへ公開することで、異種環境を上位Policyから扱える。この設計は、複数Vendor・複数Domain・Cloud APIを上位Orchestrationから連携させるONUGの方向性と一致する。

標準IPsecを共通接続手段とする点も適合する。Agentを導入できない既存拠点であっても、IX2215などの標準IPsec対応機器をHubへ接続し、Ibukiからは制御範囲の限られたLegacy/External Nodeとして扱う設計により、全面置換を前提とせず段階的に参加させられる。

### 6.2 現在の実装で確認できる部分

現在のIbukiでは、YAMLからNode、Intent、Path、Tunnel、VPP Edgeを読み込み、`explicit`、`priority`、`evaluated`の各方式でPathを選択できる。PathのAdministrative Priority、RTT、Packet Loss、Hop Countなどを比較し、Health状態やCapability条件を満たさない候補を除外できる。

AgentによるPing測定からRTT、Loss、Jitter、連続成功・失敗をTelemetryとして生成し、Controllerの再評価へ入力する経路も実装されている。イベント入力、設定再読込、状態復元、Path再選択、Explain JSONL出力、strongSwan/VPP向けRuntime Plan生成を接続しており、少なくともSD-WAN 2.0の基礎となるPolicy、Telemetry、Path Selection、障害時再選択、可視化用ログの流れを確認できる。

ただし、現時点で実通信として確認されている範囲と、API境界または計画生成だけが存在する範囲は分けて扱う必要がある。Explainログが実装済みであってもGUI Dashboard自体は未実装であり、VPP Binary APIの全操作、Graceful Transition、Flow Preserve、Cloud VPN Backend、Controller Federationも完成済みとはいえない。

### 6.3 部分的に対応する部分

Hybrid Multi-Cloudについては、Cloud VPNをAdapterとして追加できる構造とNode Capabilityの基礎はあるが、AWS、Azure、Google CloudなどのAPIを利用して接続を生成、削除、監視するBackendは未実装である。そのため、設計上は拡張可能でも、ONUGが示すMulti-Cloud AttachmentやIntegrated Cloud APIsを満たしたとはいえない。

複数管理Domainについても、Controller ID、Domain ID、Certificate、Authorized Peer、Allowed Operationを用いるController Federationの方針はある。しかし、Remote Controllerが相手DomainのAgentを直接操作せず、Controller間で要求と結果を交換する具体的Protocol、競合解決、障害時の責任範囲、3 Domain以上の調停は今後の課題である。

Securityについては、IPsec、Waypoint制約、認証されたController間通信という基礎設計を持つ一方、Firewall、IDS/IPS、SWG、CASBなどのSecurity Serviceそのものを提供するわけではない。Ibukiが担うのは、それらを配置した拠点を必須Waypointとして選択する制御であり、Security製品の機能を代替するものではない。

### 6.4 現時点で不足する部分

ONUG SD-WAN 2.0全体と比較すると、Ibukiには次の領域が不足している。

- AWS、Azure、Google Cloud等への実Cloud AttachmentとCloud API連携。
- SaaS/IaaS Applicationを識別し、Application単位でPolicyを適用するL7分類。
- 複数回線を同時利用するActive-Active Forwardingと負荷分散。
- End User端末から利用するSD-WAN Client。
- Zero Touch Provisioning、証明書Lifecycle、装置Inventoryを含む大規模運用機能。
- Site、Application、VPN単位の統合GUI Dashboard。
- SIEMや外部運用基盤へ接続する標準Northbound API。
- FIPS等の暗号Module認証や製品としてのCompliance。
- 異なるSD-WAN製品間で相互運用する標準化済みPolicy/API。

これらは、Path Transition Modelの妥当性を研究するためにすべて必須というわけではない。しかし、「SD-WAN 2.0準拠製品」と主張する場合には不足している。特にMulti-Cloud、Application Awareness、Active-Active、End User Clientは、Ibukiの現在の研究範囲を越える。

## 7. 対応状況の要約

| ONUG SD-WAN 2.0の方向性 | Ibukiの対応 | 状況 |
|---|---|---|
| 複数PathとPolicy-based Steering | Intent、Path Selection、Priority、評価値 | 実装済み |
| Healthに基づく障害回避 | Agent Telemetry、Health、Fallback候補選択 | 基礎実装済み |
| Secure Overlay | strongSwan/IPsecを中心とするTunnel制御 | 実装・評価中 |
| Security Policyとの統合 | Required Waypoint、Forbidden Waypoint | 設計・拡張対象 |
| 異種装置・Backendの収容 | Adapter/Capability Model | 基礎実装済み |
| 可視化と説明可能性 | Explain JSONL | Backend実装済み、GUI未実装 |
| 既存装置との共存 | 標準IPsecによるHub接続、Legacy Node | 設計済み |
| Multi-Domain Orchestration | Controller Federation | 設計段階 |
| Multi-Cloud Attachment | Cloud VPN Adapter | 将来計画 |
| Integrated Cloud APIs | Cloud固有Adapter/API | 未実装 |
| Application-aware Routing | 現状は主にIP Prefix・VLAN単位 | 未実装 |
| Active-Active利用 | 現状は主にActive/Standby切替 | 未実装 |
| SD-WAN Client for End Users | 研究対象外 | 未対応 |
| ZTP・大規模運用・Compliance | 製品運用機能 | 未対応 |

## 8. 結論

Ibukiの設計思想は、ONUG SD-WAN 2.0が重視するPolicy-driven Connectivity、複数Path、Security、可視性、異種環境、上位Orchestrationという方向性と整合する。特に、Pathを第一級の制御対象とし、HealthとPolicyに基づいて選択し、Adapter/Capabilityによって異種Backendを扱い、Explainログで判断を追跡する設計は、SD-WAN 2.0へ発展可能な基礎となる。

一方で、ONUG SD-WAN 2.0はHybrid Multi-Cloud、Cloud API、Application Awareness、End User Client、複数Domain連携まで含む。Ibukiはこれらをすべて実装した製品ではなく、現時点で「ONUG SD-WAN 2.0準拠」と表現することは適切ではない。

したがって、Ibukiの位置付けは次のように記述するのが正確である。

> Ibukiは、ONUGが示すSD-WAN 2.0の全機能を実装した製品ではない。一方で、複数Pathの管理、PolicyおよびHealthに基づくPath Selection、標準IPsecによる異種装置との接続、Security Waypoint制約、状態を伴うPath Transition、ならびに異種Backendを収容するAdapter/Capability Modelを備える。このためIbukiは、SD-WAN 2.0の中核的な設計思想と整合し、将来的なHybrid Multi-Cloudおよび複数管理Domainへの拡張を可能にするPath制御研究基盤として位置付けられる。

短くまとめる場合は、**「設計思想としては適合、Architectureとしては部分適合、SD-WAN 2.0製品要件としては未達」**と表現できる。
