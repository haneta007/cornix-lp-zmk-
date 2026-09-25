# Nape Pro → Prospector → Cornix 引き継ぎ

## 状態と安全な切り戻し

作業ブランチは `feat/nape-pro-ble-bridge`。`dev` と既存の `cornix_prospector_dongle_nosd` artifact は残している。Prospectorへ自動書き込みはしていない。Nape Pro本体のファームウェアも変更していない。

変更前の `dev` ベースラインは [Actions run 35829390895](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35829390895) で、Prospector、dongle用Cornix Left、Cornix Rightを含む全jobが成功した。旧Prospector UF2のSHA256は `C43C7E0717F25A14D604F4BF64E92BAF18C6D280F0176BBECC8C63E160A472AC`。ローカル退避先は `firmware/baseline-dev-35829390895/cornix_prospector_dongle_nosd.uf2`。このUF2を手元にも長期保管しておくと、Actionsの保存期限後も切り戻せる。

問題があれば、Prospectorだけをブートローダーモードにして、旧 `cornix_prospector_dongle_nosd.uf2` を手動でコピーする。Cornix左右の通常ファーム書き戻しは不要。標準UF2の再書き込みでは保存済みBLE設定・bondは消去されないため、復帰UF2でも同じ症状が残る場合がある。`dev` の `build.yaml` とファームは変更していない。

## 構成

```text
PC ← USB keyboard + mouse HID ← Prospector / XIAO nRF52840
                                ├─ ZMK split BLE → Cornix Left
                                ├─ ZMK split BLE → Cornix Right
                                └─ BLE HID client → Keychron Nape Pro

Nape Report notification → HID Report Map parser → Zephyr virtual input
                         → ZMK input listener → USB mouse HID
                                            └→ temporary layer processor
                                                NAPE_MOUSE / 700 ms
```

Nape BLE callbackは通知を固定長キューへコピーし、system work queueでReportを解析してvirtual inputへ渡す。移動X/Yのみ専用input listenerへ通し、ZMK既存のTemporary Layer Input Processor `zip_temp_layer` がレイヤー10を有効化し、最後の移動から700ms後に解除する。wheelとbuttonは別listenerへ通すため、デフォルトではタイマーを延長しない。既存の手動レイヤーには触れない。

BLE接続数は既存 `CONFIG_BT_MAX_CONN=7` と `CONFIG_BT_MAX_PAIRED=7` を維持した。split peripheral数は2のまま。Napeはsplit peripheralとして数えない。Nape scanはCornix左右のsplitサービス検出後だけ開始し、10秒で停止する。未発見時と切断時は最大32秒までの指数backoffで再試行する。split再接続時にはZMKがNape scanを中断し、競合中はsplit scanを短時間後に再試行する。

## 変更ファイル

| 範囲 | 内容 |
| --- | --- |
| `build.yaml` | 既存artifactを残し、通常版・RTTデバッグ版・split安定性診断版のProspector artifactを追加 |
| `config/west.yml` | 調査時点の依存commitを固定し、専用ZMK/Prospector module commitを参照 |
| `zephyr/module.yml`, `CMakeLists.txt`, `Kconfig` | Nape Bridgeを専用shieldだけでビルド |
| `boards/shields/cornix_nape_bridge/`, `dts/bindings/input/` | virtual input 2台、listener、700ms processor、BLE設定 |
| `config/cornix_nape_bridge.keymap` | 既存keymapを取り込み、全キーtransparentの `NAPE_MOUSE` layerを末尾へ追加 |
| `config/nape_split_stability.conf` | ZMK #3156の診断用に、Prospector側のsplit battery fetchingを無効化 |
| `nape_bridge/bridge.c`, `hid_mouse.[ch]` | scan、bonding/security、HOGP GATT discovery、Report Map解析、input注入 |
| `nape_bridge/tests/`, `.github/workflows/nape-parser.yml` | Report IDあり/なし、X/Y、wheel、buttons、異常長を検証 |
| `.github/workflows/build.yml` | 再利用ビルドworkflowをベースラインcommitへ固定 |

専用ZMK fork `haneta007/zmk` の `feat/nape-pro-ble-bridge` はsplit scanの調停とsplit以外の接続の除外だけを追加した。Prospector module `haneta007/prospector---Zmk-module` の同名ブランチはNape接続をsplit画面状態へ混ぜない変更だけを含む。巨大なZephyr forkは作っていない。

## ビルドとUF2

GitHubのfeature branchで **Build ZMK firmware** workflowを実行する。成功したrunの `firmware` artifactに以下が入る。

通常版とデバッグ版の確認runは `35838121281`（全12 job成功）。Cornixの「接続済み」表示後に入力しない症状の診断artifactを追加したcommitは `a30a1e02abea3307b9d61b2fd5eb697fb9a1c12e`。[Actions run 35890038913](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35890038913) は全13 job成功し、診断UF2を `firmware/split-stability-35890038913/`（Git管理対象外）へ展開済み。

- `cornix_prospector_nape_bridge_nosd.uf2`：通常使用するProspector版。**これだけをProspectorへ手動で書く。**
- `cornix_prospector_nape_bridge_debug_nosd.uf2`：SWD/RTTでBLEとHIDの詳細ログを採る検証版。通常版の代わりにProspectorへ手動で書く場合だけ使用。
- `cornix_prospector_nape_bridge_split_stability_nosd.uf2`：Cornix左右が「接続済み」表示なのに入力しない時の診断版。Prospectorだけに手動で書く。ZMK #3156のsplit GATT discovery競合を避けるため、Prospector上のCornixバッテリー取得/プロキシを無効化する。
- `cornix_prospector_dongle_nosd.uf2`：従来Prospector版。
- `cornix_left_for_dongle_nosd.uf2`、`cornix_right_nosd.uf2`：既存Cornix左右版。今回の機能のための再書き込みは不要。

診断版ではProspector画面のCornixバッテリー残量が更新されない。通常版と既存artifactは変更せず残す。

ローカルUF2のSHA256：

| UF2 | SHA256 |
| --- | --- |
| `cornix_prospector_nape_bridge_nosd.uf2` | `DF054D59F317C46346F19FCDCE15BE6BED6963F28AE8BE2C8117D96F1A798D0F` |
| `cornix_prospector_nape_bridge_debug_nosd.uf2` | `795FC10EA13F1458B6EFD3801F78AF8206E91EF9854842FD3FF28F06ACDA397F` |
| `cornix_prospector_nape_bridge_split_stability_nosd.uf2` | `348C40856E53AC9132561D97EBF9EE7AA7C27C501CDAF800CCB18496CC77CE3B` |

新しい診断UF2のローカルパスは `firmware/split-stability-35890038913/cornix_prospector_nape_bridge_split_stability_nosd.uf2`。

feature branchの従来名 `cornix_prospector_dongle_nosd.uf2` も生成されるが、依存ZMKにsplit scan調停の小変更が入るため、変更前の完全な切り戻しには冒頭のベースラインUF2（SHA256 `C43C...`）を使う。

新しいビルドのcommitは `config/west.yml` に固定した。ベースラインのZMKは `9ebbeff0a8b69a42f14aec022cdf16c7a107b9e0`、Zephyrは `10ba6d0cb38bc3d258775d27982f707599320085`、Prospector moduleは `ed98221f3b52b7066dbb10ba3af8a29150b93a5a`。依存を浮動の `main` のまま更新していない。

## Nape Proのペアリング

1. ProspectorのUSBをPCへ接続し、Cornix LeftとRightが両方接続されるまで待つ。
2. Nape Proの側面スイッチを `BT` へ切り替える。既にPCとペアリング済みのBluetoothチャンネルではなく、空いているチャンネルを選ぶ。
3. Nape Proの丸いFnボタンと `1` / `2` / `3` のいずれかを約4秒押してペアリングモードにする。青いLEDの点滅を確認する。[KeychronのNape Pro案内](https://www.keychron.co.th/blogs/tutorial/nape-pro-nape-pro-quick-start-guide)に基づく操作で、製品の版によって表記が違う場合は付属マニュアルを優先する。
4. Prospectorは広告名に `Nape Pro` を含むデバイスを探索し、接続後に暗号化・HID service discovery・Report Map read・Input Report購読を進める。PC側のBluetooth設定でNapeをペアリングしない。
5. ペアリングが済めばProspector側settingsにbondが保存される。Napeが見つからない時は10秒のscan窓を挟んで再探索するため、点滅中にすぐ反応しない場合はしばらく待つ。

## 初回テスト

1. 旧UF2のバックアップを確認してから、通常版 `cornix_prospector_nape_bridge_nosd.uf2` をProspectorへ**手動**で書く。Cornix左右は現状維持。
2. Napeの電源を切ったまま、Cornix左右で通常の文字入力を確認する。
3. Napeを上記手順でペアリングし、ボールのX/YでPCポインタが動くこと、wheelのスクロール、左/右/中クリックを確認する。
4. ボールを動かすとProspectorのレイヤー表示が `NAPE_MOUSE` となり、止めて約700ms後に元へ戻ることを確認する。wheelのみ、buttonのみでは延長しないことも確認する。
5. Napeだけ電源OFFにして、Cornixの文字入力が続くことを確認する。その後Napeを戻し、backoff後に再接続することを確認する。
6. Cornix片側を一時的にOFF/ONし、Nape scanよりsplit再接続が優先されることを確認する。

## Cornixが接続表示なのに入力しない時

今回の症状は、複数split peripheralと `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` の組合せで、BLE接続表示は出てもキー通知用position-stateのGATT購読が欠落するZMK [Issue #3156](https://github.com/zmkfirmware/zmk/issues/3156) の条件に一致する。根本修正案 [PR #3411](https://github.com/zmkfirmware/zmk/pull/3411) は調査時点で未mergeのため、診断artifactはbattery fetching/proxyを無効化して競合を回避する。Prospector画面のCornixバッテリー残量はこの版では更新されない。

1. Nape Proの電源を切る。
2. `cornix_prospector_nape_bridge_split_stability_nosd.uf2` をProspectorだけに手動で書く。Cornix左右には書かない。
3. Prospectorを起動し、Cornix Leftを接続して入力を試す。次にRightを接続して両側を試す。
4. 入力が戻ればsplit GATT discovery競合の可能性が高い。戻らなければこの原因と断定せず、RTT debug logで `Found position state characteristic` と `[SUBSCRIBED]` を確認する。

この文書作成時点では**Nape Pro実機のReport Map取得・実機ペアリング・PCカーソル・画面の表示は未検証**。CIはコードとUSB HID構成のコンパイル検証であり、実機での成功を意味しない。

## ログとトラブルシューティング

通常版は大量ログを無効化する。デバッグ版は `CONFIG_ZMK_NAPE_DEBUG=y` とSEGGER RTT backendを使う。SWD/RTT対応プローブでログを読む。Windows常駐アプリは通常動作に不要。主な行は `NAPE: scan start`、`candidate found`、`connected`、`security established`、`HID service found`、`report map read`、`subscribed report id=X`、`input ...`、`disconnected`、`reconnect scheduled`。Report Mapと未知のInput Reportはデバッグ版でHEX dumpする。

### 実機ペアリング失敗時のUSBログ診断版

2026-09-24の実機観察では、Nape対応版をProspectorへ入れてCornix左右の文字入力は動作した。WindowsのBluetooth一覧には `Keychron Nape Pro` が表示され、Napeの青いペアリング点滅は接続せずに終了した。Prospector側のscan・接続・認証のどこで止まるかは未確認。

この切り分け用に `cornix_prospector_nape_bridge_usb_log_nosd.uf2` を追加した。[Actions run 35893902660](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35893902660) は全14 job成功し、USBログ版のRAM使用量は `243336 / 262144` バイト（92.83%）。UF2は `firmware/nape-usb-log-35893902660/cornix_prospector_nape_bridge_usb_log_nosd.uf2`、SHA256は `9A53F7D55A595C360E587FE5F8CFA192DD8B90257B69034E28D6FF94DB7FEC5D`。Cornix左右のUF2 hashは前回と同一。

1. この診断UF2を**Prospectorだけ**に手動で書く。Cornix左右の入力を確認する。この版はUSB CDCポートを文字ログ専用にし、ZMK Studio RPCを無効化する。通常版・RTT版・復帰版のartifactは残している。
2. Windowsで追加されたCOMポートをシリアル端末で開く。115200 bpsを指定し、必要ならDTRを有効にする。ログ取得を始めてからNapeをBTの空きチャンネルでペアリング点滅させ、少なくとも1分記録する。通常利用にWindows常駐アプリは不要。
3. `NAPE: bridge initialized`、`waiting for Cornix split discovery`、`scan start` または `scan start failed`、`candidate found`、`connected`、`security established` の最終到達点を確認する。`scan start` が繰り返されるのに `candidate found` が無ければ広告名/形式を調べる。`candidate found` の後に失敗すればBLE接続/認証を調べる。
4. ログ取得後、Prospectorへ通常のNape対応版または旧dongle版UF2を手動で戻す。

USBログ版はCIでビルドと設定（`CONFIG_ZMK_USB_LOGGING=y`、`CONFIG_LOG_BACKEND_UART=y`）を確認した。実機のUSB列挙・ログ採取・NapeのBLE接続も確認した。HID入力の転送は未確認。

2026-09-24の実機ログでは、WindowsのCOM20からログを取得できた。Napeは `candidate found` → `connected` → `security established (level 2)` まで進んだが、その後 `HID service missing` となった。約18秒後にNape側から切断され、次の接続では `security request failed (-12)` が出た。Cornix左右の文字入力は動作している。これらは「Napeを発見できない」「BLE接続ができない」という原因を否定するが、NapeにHIDサービスが本当に無いのか、UUID指定のGATT探索だけが失敗したのかは、このログだけでは区別できない。`-12` の原因も未確定。

そのため次の診断版では、Napeの全primary GATT serviceを一度列挙してUUIDとhandle範囲を記録する。0x1812（HID Service）があれば後続のReport Map探索を続ける。再接続時のsecurity要求が失敗した場合は、その時点のbond数もログへ出し、接続を切ってbackoffへ戻す。Cornix側のbondは消去しない。

この診断版は [Actions run 35896479036](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35896479036) の全14 jobでビルド成功。書き込み対象は `firmware/nape-gatt-services-35896479036/cornix_prospector_nape_bridge_usb_log_nosd.uf2`（SHA256 `47912C54F53EB21D8A40739077D60A885F9F6CE393A455E354DBD0E7488163E4`）。**Prospectorだけ**へ手動で書く。Cornix左右のUF2は前回の診断runとSHA256が一致し、再書き込みは不要。リンク時RAMは `243336 / 262144` バイト。現時点では新しい診断版の実機GATT一覧は未取得。

書き込み後はCOM20などのUSBログポートを開いてからNapeをペアリング点滅させ、`NAPE: GATT service 0x....`、`NAPE: HID service found` または `NAPE: GATT discovery ended without HID service`、`NAPE: security request failed ... bonds N/7` を記録する。ポート番号は再列挙で変わる場合がある。

上記診断版の実機ログ（2026-09-24）では、Nape検出とBLE接続は繰り返し成功したが、毎回 `NAPE: security request failed (-12), bonds 7/7` となりGATT列挙前に切断した。固定済みZephyrの `smp_send_pairing_req()` は鍵スロットを取得できない時に `-ENOMEM` を返すため、この時点の直接の阻害要因はbond枠満杯と判断した。Cornixの既存bondを消す操作はしていない。

Nape shieldだけ `CONFIG_BT_MAX_PAIRED=8` とした修正版は [Actions run 35924820087](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35924820087) の全14 jobで成功。USBログ版の実際のKconfig値は8、リンク時RAMは `243592 / 262144` バイト（92.92%）。次にProspectorへ**手動で書くUF2**は `firmware/nape-bond-slot-35924820087/cornix_prospector_nape_bridge_usb_log_nosd.uf2`、SHA256 `1FEB8D34EAFAB140E0E3433C635F52D10B18C4EC5206CF7F7938A09DB5D78ECD`。従来Prospector版とCornix左右のUF2は前runと完全一致。Nape対応の通常版も生成されたが、GATTサービスとReport Mapが未検証なので現時点ではUSBログ版で診断を続ける。

Zephyrソースを確認すると `bt_foreach_bond()` は現接続ではなく、鍵データを保存した相手だけを列挙する。そのため `7/7` は実際に有効な7件で、単なる「今接続中2台」との数え違いではない。誰の鍵かは現在のログに出していない。次のUSB診断版ではCornix接続後・Nape scan開始前に、保存済みbondと接続中LE peerのアドレスを一度だけ表示する。両方のCornixアドレスと照合し、残りが古い相手かNapeの過去ペアリングかを見分ける。アドレスログは診断版だけで有効化する。

bond内訳診断版は [Actions run 35928386017](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35928386017) の全14 jobで成功。UF2は `firmware/nape-bond-inventory-35928386017/cornix_prospector_nape_bridge_usb_log_nosd.uf2`、SHA256 `7379CF989EAD224F2A251BD1CA14EBA4A0FD5886E37B715615786334A8870499`。実際の設定は `CONFIG_BT_MAX_PAIRED=8` と `CONFIG_ZMK_NAPE_BOND_DIAGNOSTICS=y`、RAMは `243592 / 262144` バイト。通常のProspector版とCornix左右UF2のSHA256は直前runと一致する。診断ログにはBLEアドレスが含まれるため、ローカルで読み取ってCornix接続先と照合し、不要なaddress情報は引き継ぎ文書へ保存しない。

2026-09-25の実機でbond一覧を採取。保存bondは7件、起動時の接続中LE peerは2件。接続中2アドレスは保存bondのindex 5と6に完全一致し、Cornix左右のbondと特定できた。残るindex 0〜4の5件はその時点で未接続。index 1は以前のログにあるNape接続先アドレスと完全一致。index 2は別ログにあるNape candidate群とアドレスprefixが一致し、index 4はindex 1とprefixが一致するためNapeのアドレス変化による古いbondの可能性が高い。index 0と3の相手はログから特定できず、他の古いpeerかどうかは未確定。`CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START=n` のため、切断や通常再起動ではこれらのbondは消えない。既存bondの削除は行っていない。

#### Nape候補bondだけを削除する一回限りの手順

2026-09-25のinventory logと、過去のNape接続ログを照合した結果、index 1はNapeの保存bondと一致し、index 2と4はNapeのアドレス変化候補と判断した。相手を特定できないindex 0と3、およびCornix左右のbondは削除対象にしない。

この3件だけを対象にする `cornix_prospector_nape_bond_cleanup_nosd` artifactを追加した。cleanup-only版はCornix左右のsplit ready後に、保存bond数が4〜8、接続中LE peerがCornix左右の2件、対象fingerprintが重複せずactive peerではないことを確認してから個別に `bt_unpair()` を呼ぶ。条件が一致しなければ何も削除しない。MAC addressそのものはコード・ログへ出さない。削除処理後にNape scanを開始しないため、このUF2を入れたままではNapeと再pairingしない。

Actionsでこのartifactのbuildが成功した後、次の順に手動で行う。実機への自動flashは行わない。

1. Cleanup UF2をProspectorだけに書く。Cornix左右は変更しない。
2. USB CDCログを開いた状態でCornix左右を接続し、`NAPE: bond cleanup complete removed=3 failed=0 remaining=4 active LE=2` を確認する。条件がずれている場合は `bond cleanup aborted` が出てbondは削除されない。
3. Cleanup UF2の役目はここまで。Napeの再接続診断には、既存の `cornix_prospector_nape_bridge_usb_log_nosd.uf2` をProspectorへ手動で戻す。この診断UF2はbond inventoryにraw addressを記録するので、ログはローカルで扱い未加工のまま共有しない。
4. NapeをBTモードの未使用チャンネルでFn+1を約4秒押してpairing点滅させ、ログの `candidate found`、`connected`、`security established`、GATT service列挙のどこまで進むかを記録する。

このbond cleanupはProspectorに保存されたNape候補だけに作用する。Windows側のBluetooth登録、Nape本体のfirmwareや設定、Cornix左右のbondには作用しない。bond枠の解放だけではHID接続を検証できない。後述の実機動作確認では、別途Nape入力のUSB転送とlayer切り替えが確認された。

2026-09-26の[Actions run 36182378734](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36182378734)で全13 build jobとartifact mergeが成功した。runの `firmware` artifact（3,328,324 bytes）にcleanup版を含むUF2一式がある。artifact archiveのSHA256は `C4772D7952C12FCE23D9B4B6B90E1A4B0E5CAD26D510ACCAFA8FE3F1E5A7AA94`。これはZIP archiveのhashで、個別UF2のhashではない。GitHub Actionsから `firmware` をダウンロードして展開し、`cornix_prospector_nape_bond_cleanup_nosd.uf2` だけをProspectorへ手動で書く。個別UF2のhashと、後述の実機テストに使ったUF2名は未確認。

同じ診断ログの再起動前に、既知Nape bondと一致するアドレスで接続し `security established (level 2)` まで成功。その後primary GATT列挙は約4秒で `GATT discovery ended without HID service (0 services)` となり、接続timeout reason `0x08` で切断した。診断実装はZephyr GATT callbackの終端をサービス0件として数えたが、固定済みZephyrはATT discovery error時にも同じNULL終端callbackを呼ぶため、Napeにサービスが存在しないと断定できない。bond枠の不足は新規アドレスでのペアリングを妨げていたが、既存bondで接続できた場合にもGATT discovery問題が別途残る。

### 2026-09-26の実機動作確認

ユーザーから、Nape ProがProspectorに接続し、ボール操作でPCのポインタが動くこと、`NAPE_MOUSE` layerへ切り替わって約700ms後に戻ること、wheelと各mouse buttonが反応することを確認したとの報告があった。これにより実機でのBLE入力からUSB mouse HID、Auto Mouse Layerまでの動作が確認できた。前項のGATT discovery失敗は後の動作確認で実用上解消している。

確認に使用した正確なUF2名、bond cleanupログ、Nape Report Mapのraw dumpは記録されていない。従って個々のbond削除結果とdescriptorの内容は未確認のまま。実機動作結果はユーザーによる直接確認報告であり、CIだけから推定したものではない。

- `scan start` が出ない：Cornix左右のsplit接続とGATTサービス検出を先に確認する。
- `candidate found` が出ない：NapeのBTモード、ペアリング点滅、広告名を確認する。必要なら `CONFIG_ZMK_NAPE_NAME` を変更する。
- `security established` が出ない：Napeの別Bluetoothチャンネルを試し、古い相手とのbond状態を確認する。Cornixのbondを不用意に一括消去しない。
- `report map read` の後に購読できない：デバッグ版でReport MapとReport Referenceを採取し、parserの対応範囲を確認する。実機descriptorを推測で固定しない。
- ポインタは動くがレイヤーが変わらない：`config/cornix_nape_bridge.keymap`が選択され、ProspectorにNape版UF2を書いたか確認する。
- 入力が詰まる：`NAPE: input queue full`、RAM使用量、BLE接続の切断ログを確認する。
- Prospector画面でCornix左右が接続済みなのにキー入力できない：ZMKの複数split peripheralとbattery fetching併用時に、接続表示だけ先に出てposition-stateのGATT購読が欠ける既知の競合がある。診断版で回避する。通常版でのみ再発する場合はこの競合の可能性が高いが、実機ログでの確認は別途必要。

## 調整箇所と既知の制限

- タイムアウトは `boards/shields/cornix_nape_bridge/cornix_nape_bridge.overlay` の `<&zip_temp_layer 10 700>` の `700` を変更する。移動時だけ更新する構造はそのまま。
- `NAPE_MOUSE`は現時点で全キーtransparent。クリックはNape本体のボタンから送る。キー割当を追加する場合は `config/cornix_nape_bridge.keymap` の1レイヤーにまとめる。
- parserは相対X/Y、wheel、水平wheel、8個までのButton fieldを扱う。ZMK USB mouseへ送るボタンは先頭5個。複雑なHID Report Map、64バイトを超える1通知、512バイトを超えるReport Map、Boot Mouseだけの機器は未対応。
- Nape実機のReport Map raw dumpは保存されていない。ユーザー実機でX/Y・wheel・button転送は動作確認済みだが、descriptorの内容や他機種への汎用性は未確認。未知のReportは通常版でUSBへ転送しない。
- 最終CIのリンク時RAMは通常版 `256770 / 262144` バイト（97.95%、残り5374バイト）、デバッグ版 `259714 / 262144` バイト（99.07%、残り2430バイト）。旧Prospectorのベースラインは `252730 / 262144` バイト（96.41%）。これは静的配置と設定済みスタックの値であり、BLE 3接続時の実際のスタック余裕や連続稼働は未測定。特にデバッグ版は余裕が小さいため短時間のRTT調査用とし、通常運用は通常版を使う。
- Bluetooth認証方式とレポート内容はNape本体の実機・ファーム版に依存する。実機で不適合が判明した場合は、HEX dumpを根拠に小型parserへ限定的に対応を追加する。
