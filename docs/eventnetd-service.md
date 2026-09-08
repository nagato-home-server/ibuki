# eventnetd Service Deployment

`deploy/ibuki-eventnetd.service`は、eventnetdをLinuxのsystemdサービスとして起動するための最小unitです。起動前にバイナリの実行権限とYAMLのreadable状態を検査します。これは本番環境へそのまま適用する完成品ではなく、配置先と権限を環境に合わせて確認するためのテンプレートです。

## 前提

- `/usr/local/libexec/ibuki/eventnetd`へLinuxビルド済みバイナリを配置する。
- `/etc/ibuki/ibuki.yaml`を`ibuki`だけが読める権限で配置する。
- `ibuki`ユーザーと`ibuki-agent`ユーザーを作成する。
- `ibuki`ユーザーに、strongSwanのVICI socketとVPP CLI socketを操作する権限を与える。Agent UID制限を使う場合は、unitの`ExecStart`へ`--socket-uid`と`id -u ibuki-agent`の数値を追加する（systemd unitはコマンド置換を行わないため、文字列のユーザー名は指定しない）。
- YAML内のVPP接続先とstrongSwan接続先を環境の実socketへ変更する。

## 配置と起動

CMakeのinstallを使う場合は、実行ファイルとunitを同じ配置規則で展開できます。

```sh
cmake --install build --prefix /usr/local
```

この操作でunitは`/usr/local/lib/systemd/system`、eventnetdは`/usr/local/libexec/ibuki/eventnetd`、その他のCLIは`/usr/local/bin`へ配置されます。unitの`ExecStart`とeventnetdの配置先は一致しています。

```sh
sudo install -m 0644 deploy/ibuki-eventnetd.service /etc/systemd/system/ibuki-eventnetd.service
sudo systemctl daemon-reload
sudo systemctl enable --now ibuki-eventnetd.service
sudo systemctl status ibuki-eventnetd.service
```

配置ミスの場合は`systemctl status`の`ExecStartPre`失敗として確認できます。unitは`/usr/bin/test`の配置を前提にしているため、ディストリビューションでパスが異なる場合は環境のabsolute pathへ変更してください。

Agentは`/run/ibuki/eventnetd.sock`へJSONLを送ります。unitは`--socket-accept-count 0`で逐次接続を継続し、切断時は3回までbackoff付きで再接続を受け付けます。複数Agentを同時に処理する場合は、有限接続の`--socket-parallel`評価を常駐serviceへ移す前に、poll timeout、認証、負荷上限を追加検証してください。

unitは`PrivateDevices`、kernel／control group保護、SUID/SGID制限、personality固定、`AF_UNIX`／`AF_INET`／`AF_INET6`以外のaddress family制限も有効にしています。`RuntimeDirectory`／`StateDirectory`、`UMask=0077`、`ReadWritePaths`により、runtime socket・state fileの配置と書込み範囲も固定しています。VICI／VPP socketや実行環境で追加のfamily・deviceが必要な場合は、無制限に緩和せず、必要な設定だけを明示的に追加してください。

## 検証

```sh
sudo systemctl is-active --quiet ibuki-eventnetd.service
sudo test -S /run/ibuki/eventnetd.sock
sudo journalctl -u ibuki-eventnetd.service -n 50 --no-pager
sudo stat -c '%a %U %G' /var/lib/ibuki/state.tsv
```

論文評価ではsystemdを必須にせず、既存の`vm-evaluate.sh`とmock backendを使用します。unitは安全のため`--apply`なしのdry-runで始まります。VICI／VPPの権限・socket接続・生成コマンドを確認し、明示的な承認後に`ExecStart`へ`--apply`を追加して本番投入します。systemdのSIGTERM／SIGINT停止時は、現在のreconcile完了後に終了し、atomic replace済みのstateを次回起動で復元します。
