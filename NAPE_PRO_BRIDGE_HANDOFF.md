# Nape Pro → Prospector → Cornix 引き継ぎ

## 2026-09-29 FN_SCROLLと慣性スクロール

`feat/nape-scroll-inertia`では既存のlayer番号を再利用し、Layer 5を`FN_SCROLL`、Layer 6を`NAPE_MOUSE`、Layer 7を両者が同時に有効な時の競合解消に使う。旧Layer 8/9はtransparentなlegacy layerとして保持する。Layer 1のFN bindingsとsensor-bindingsはそのまま残し、FN keyboard操作をLayer 5へ複写した。Base Layer 0の無変換・Enterにある`&lt150`はタップ時の無変換/Enterと150msのtap-hold設定を保ったまま、ホールド先をLayer 5に変更した。

Layer 6は既存のNape mouse bindingsと700ms temporary-layer timeoutを維持する。Layer 7はLayer 5と6の双方がactiveな時だけ有効になり、マウスbuttonと競合する位置19/20/21をFNの`N5`/`N6`/`KP_PLUS`として解決する。他の位置はtransparent。既定レイヤー優先順位ではLayer 7が6より優先されるため、NAPE_MOUSEが残った状態でFN_SCROLLを押してもこの3キーのFN操作を通す。

Layer 5中はNape XYを既存の`zip_xy_to_scroll_mapper`と`zip_scroll_scaler 1/8`へ通し、慣性trackerは入力を通過させながら速度だけ記録する。停止40ms後に閾値を超える速い動きだけ、別のvirtual input deviceから慣性wheel eventを出す。system workqueue上のdelayable workを16ms周期で使い、Q8 EMA `(old×3 + sample)/4`、方向反転時の速度リセット、減衰230/256、最低開始速度8 raw counts/tick、停止閾値1 count/tick、最大1200ms、軸ごとのfractional remainderを使用する。慣性listenerにはXY mapperとtemporary-layer processorがないためLayer 6の700ms timerに入らない。Layer 5解除時とNape切断時は速度・端数を消してworkをキャンセルし、work側でもLayer 5を再確認する。

固定commitのZMK/Zephyr依存は更新していない。`Nape HID parser` run [36494844006](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36494844006)はinertia math (ASan/UBSan)・parser・layer validatorが成功。`Build ZMK firmware` run [36494844832](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36494844832)はNape variant、Cornix Left/Right、rollbackを含む全matrix buildに成功した。レイヤー検証では無変換とEnterの両方がLayer 5を指すことも確認した。

Prospector通常Nape版のRAM/Flashは、変更前のLayer 8 scroll buildで247,290 / 262,144 bytes (94.33%)・582,056 bytes、慣性とlayer migration追加後で247,546 / 262,144 bytes (94.43%)・584,124 bytes。差分はRAM +256 bytes、Flash +2,068 bytes。変更前値は固定west manifestによる比較buildのZephyr memory-region report、変更後値は同一設定の通常Nape build reportから取得した。

最新の通常UF2は[firmware run 36494844832](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36494844832)の`firmware` artifact内にある`cornix_prospector_nape_bridge_nosd.uf2`。ローカル保管先は`firmware/nape-scroll-inertia-36494844832/cornix_prospector_nape_bridge_nosd.uf2`、サイズ1,168,384 bytes、SHA256 `FF55C6E37BCFBF3E27B0DFF362A022CD21AA0EF8F1B4ADCE165205A53B68501E`。Prospectorだけに手動で書く。自動flashはしていない。実機では無変換/Enterのtap-hold、FN操作と同時のスクロール、fling後の慣性、Layer 5解除直後の停止、Layer 6残留時のFN操作、button/wheel、左右split typingを確認する。40ms開始遅延と慣性係数は実機操作感で調整する。

## 状態と安全な切り戻し

Nape BLE bridgeの基準実装は `feat/nape-pro-ble-bridge`。本作業はそこから続く `feat/nape-scroll-inertia` で行い、`feat/nape-scroll-modifier`、`dev`、既存の `cornix_prospector_dongle_nosd` artifactは変更・削除していない。Prospectorへ自動書き込みはしていない。Nape Pro本体のファームウェアも変更していない。

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
                                                NAPE_MOUSE / Layer 6 / 700 ms
```

Nape BLE callbackは通知を固定長キューへコピーし、system work queueでReportを解析してvirtual inputへ渡す。通常時の移動X/YはZMK既存のTemporary Layer Input Processor `zip_temp_layer` でLayer 6（`NAPE_MOUSE`）を有効化し、最後の移動から700ms後に解除する。Layer 5（`FN_SCROLL`）が有効な間は同じX/Yをscroll mapperへ通す。wheelとbuttonは別listenerへ通すため、デフォルトではタイマーを延長しない。

Nape専用Prospector variantは `CONFIG_BT_MAX_CONN=4`（Cornix左右、Nape、予備1）と `CONFIG_BT_MAX_PAIRED=8` を使う。通常ProspectorとCornixの設定は変更しない。split peripheral数は2のままで、Napeをsplit peripheralには数えない。ZMKでは `ZMK_SPLIT_BLE` が `ZMK_BLE` に依存するため、このvariantでもZMK BLE機能全体は無効化できない。PC出力はUSBを使い、ZMK BLE側の既存split動作を保つ。Nape scanはCornix左右のsplitサービス検出後だけ開始し、10秒で停止する。未発見時と切断時は最大32秒までの指数backoffで再試行する。split再接続時にはZMKがNape scanを中断し、競合中はsplit scanを短時間後に再試行する。

## 変更ファイル

| 範囲 | 内容 |
| --- | --- |
| `build.yaml`, `build-baseline.yaml` | Nape Prospector/Cornix buildと、別archiveに出す従来名のProspector rollback buildを分離 |
| `config-baseline/west.yml`, `config-baseline/cornix.keymap` | dev時点の依存SHAとkeymapを使うrollback build設定。keymapの固定SHA256をCIで確認 |
| `zephyr/module.yml`, `CMakeLists.txt`, `Kconfig` | Nape Bridgeを専用shieldだけでビルド |
| `boards/shields/cornix_nape_bridge/`, `dts/bindings/input/` | virtual input、listener、700ms processor、BLE設定 |
| `config/cornix.keymap` | Layer 5/6/7を既存枠内でFN_SCROLL/NAPE_MOUSE/競合解消に再利用 |
| `nape_bridge/inertia.c`, `inertia_math.h` | system workqueue上の慣性scroll、Layer 5・切断・新入力時の停止 |
| `config/nape_split_stability.conf` | ZMK #3156の診断用に、Prospector側のsplit battery fetchingを無効化 |
| `nape_bridge/bridge.c`, `hid_mouse.[ch]`, `input_queue.h` | scan、bonding/security、HOGP GATT discovery、Report Map解析、input注入。4件リングキューで古い通知を落とす時は押下buttonをrelease |
| `nape_bridge/tests/`, `.github/workflows/nape-parser.yml` | Report IDあり/なし、X/Y、wheel、buttons、異常長、queue overflowを検証。layer indexも検証 |
| `.github/workflows/build.yml` | Nape版とbaseline版を別archiveでbuildし、旧keymapの固定SHA256を検査。ZMK build workflowは解決済みcommitへ固定 |

専用ZMK fork `haneta007/zmk` の `feat/nape-pro-ble-bridge` はsplit scanの調停とsplit以外の接続の除外だけを追加した。Prospector module `haneta007/prospector---Zmk-module` の同名ブランチはNape接続をsplit画面状態へ混ぜない変更だけを含む。巨大なZephyr forkは作っていない。

## ビルドとUF2

Keymap Editorで `feat/nape-pro-ble-bridge` ブランチのkeymapを保存すると、**Build ZMK firmware** workflowが自動起動する。GitHubのActionsでそのrunを開き、成功後に画面下部の `firmware` artifactをダウンロードする。手動で起動する場合はActionsの **Build ZMK firmware** から **Run workflow** を選び、同じfeatureブランチを指定する。`firmware` artifactにNape版とCornix左右のbuildが入り、従来Prospector版は独立した `firmware-baseline-dev` artifactに入る。

通常版とデバッグ版の確認runは `35838121281`（全12 job成功）。Cornixの「接続済み」表示後に入力しない症状の診断artifactを追加したcommitは `a30a1e02abea3307b9d61b2fd5eb697fb9a1c12e`。[Actions run 35890038913](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/35890038913) は全13 job成功し、診断UF2を `firmware/split-stability-35890038913/`（Git管理対象外）へ展開済み。

- `cornix_prospector_nape_bridge_nosd.uf2`：通常使用するProspector版。**これだけをProspectorへ手動で書く。**
- `cornix_prospector_nape_bridge_debug_nosd.uf2`：SWD/RTTでBLEとHIDの詳細ログを採る検証版。通常版の代わりにProspectorへ手動で書く場合だけ使用。
- `cornix_prospector_nape_bridge_split_stability_nosd.uf2`：Cornix左右が「接続済み」表示なのに入力しない時の診断版。Prospectorだけに手動で書く。ZMK #3156のsplit GATT discovery競合を避けるため、Prospector上のCornixバッテリー取得/プロキシを無効化する。
- `cornix_prospector_dongle_nosd.uf2`：`firmware-baseline-dev` archiveに含まれる従来名のrollback build。依存はActions run 35829390895で解決されていたSHAに固定。これはローカル保管済み旧UF2とのbyte-for-byte一致を保証しない。完全なhash確認済rollbackには冒頭に記したローカル保存UF2を使う。
- `cornix_left_for_dongle_nosd.uf2`、`cornix_right_nosd.uf2`：既存Cornix左右版。今回の機能のための再書き込みは不要。

通常Nape版は環境光センサーによる自動調光を無効にし、固定輝度80でビルドする。設定ファイルは `config/nape_fixed_brightness.conf`。明るさを変更する場合は `CONFIG_PROSPECTOR_FIXED_BRIGHTNESS` を1〜100の範囲で編集する。自動調光へ戻す場合は `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y` に変更して固定値設定を外し、再ビルドする。この専用設定は通常版だけに適用し、デバッグ・USBログ・bond cleanup・split stabilityの各variantおよび切り戻し版には適用しない。

2026-09-28の固定輝度版は [Actions run 36363692908](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36363692908)。全18 jobが成功した。通常版の生成Kconfigには `CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=80` が出力される。この項目はProspector Kconfigで環境光センサー無効時のみ有効なため、自動調光OFFも確認できる。リンク時RAMは `247290 / 262144` bytes（94.33%、残り14854 bytes）で、直前版の `248570` bytesから1280 bytes減った。生成UF2 SHA256は `C6589F6286E510452FB04BAD8292B170F8E16028F4568F8926957B48351FD102`。通常版の画面輝度は実機で未確認なので、Prospectorに手動で書いた後、希望する見え方か確認する。

Keymap Editorの次の保存commit `c18c098` では `Nape HID parser` のみ自動実行された。原因は本firmware workflowのpush条件が `tags-ignore` のみで、ブランチpushが対象外だったこと。`branches: ["**"]` へ修正したcommit `28fe73e` のpushで [Build ZMK firmware run 36365659208](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36365659208) が自動起動し、全18 job成功した。最新keymapと固定輝度80を含む `firmware/cornix_prospector_nape_bridge_nosd.uf2` のSHA256は `9518A995A996AE298F63010CC846642D1722659CC3136540758795564C4ACD9A`。同runの `firmware-baseline-dev/cornix_prospector_dongle_nosd.uf2` は既存rollback hash `C43C7E0717F25A14D604F4BF64E92BAF18C6D280F0176BBECC8C63E160A472AC` と一致する。新UF2の実機書き込みと動作確認は未実施。

診断版ではProspector画面のCornixバッテリー残量が更新されない。通常版と既存artifactは変更せず残す。

以前の診断runで保存したローカルUF2のSHA256（現行runのUF2ではない）：

| UF2 | SHA256 |
| --- | --- |
| `cornix_prospector_nape_bridge_nosd.uf2` | `DF054D59F317C46346F19FCDCE15BE6BED6963F28AE8BE2C8117D96F1A798D0F` |
| `cornix_prospector_nape_bridge_debug_nosd.uf2` | `795FC10EA13F1458B6EFD3801F78AF8206E91EF9854842FD3FF28F06ACDA397F` |
| `cornix_prospector_nape_bridge_split_stability_nosd.uf2` | `348C40856E53AC9132561D97EBF9EE7AA7C27C501CDAF800CCB18496CC77CE3B` |

新しい診断UF2のローカルパスは `firmware/split-stability-35890038913/cornix_prospector_nape_bridge_split_stability_nosd.uf2`。

従来名のProspector buildは `firmware-baseline-dev` archiveへ分離した。これは過去に解決した依存SHAでの再buildである。2026-09-28のrunでは、保存済み旧UF2とSHA256が一致した。変更前の完全な切り戻しには冒頭のベースラインUF2（SHA256 `C43C...`）を使う。

2026-09-27の保守差分では、Prospector bridge専用の `CONFIG_BT_MAX_CONN` を7から4へ下げた。Nape通知キューは8要素から4要素にし、キューpayloadの静的領域を576 bytesから288 bytesへ減らした。これらを含む現行版のRAM計測値は後述する。`CONFIG_BT_MAX_PAIRED=8` は維持し、split中央や既存Cornix artifactには適用しない。BLE通知が4件を超えて滞留すると古い入力を破棄し、buttonが押しっぱなしにならないようにreleaseを発行する。

この作業ブランチの依存は `config/west.yml` とZMK側の `app/west.yml` で固定している。ZMK forkは `edafb3b058445329d4cbc226621eb1d37529480c`、Zephyr forkは `10ba6d0cb38bc3d258775d27982f707599320085`、Prospector moduleは `28438c476e2e17d648ce25a14d85525997cc48e3`。今回これらのrevisionは更新していない。

## Keymap EditorでFN_SCROLLとNAPE_MOUSEを編集

GitHub連携のKeymap Editorで `haneta007/cornix-lp-zmk-` の `feat/nape-scroll-inertia` ブランチを選び、`config/cornix.keymap` を開く。Layer 5は `FN_SCROLL`、Layer 6は `NAPE_MOUSE`。Layer 7はLayer 5と6の同時active時に使う競合解消用で、Layer 8/9も番号を保ったまま残る。総数は従来どおり10レイヤー。Nape版Prospectorはこの共通keymapを直接ビルドする。

`NAPE_MOUSE` の未割当キーは下位レイヤーを通す。既存のミュート・中クリックと左・中・右クリック位置を保つ。Layer 8/9は `Legacy 8` / `Legacy 9` として全位置transparentにし、前のLayer 8/9の役割は5/6へ移してある。FN入口をLayer 1からLayer 5へ移すが、Layer 1のFN配置自体はlegacy用に残す。

## FN_SCROLLキーを押している間のNapeスクロールと慣性

Base Layerの無変換とEnterにある既存layer-tapは、タップすると従来どおり無変換/Enter、ホールドするとLayer 5を有効にする。150msのhold-preferredとquick-tap設定は変えていない。専用scrollキーの追加は不要。

固定ZMK commit `edafb3b058445329d4cbc226621eb1d37529480c` の `app/src/keymap.c` を確認した。既定のレイヤー順では高い番号が先に照合されるため、Layer 7がLayer 6より優先される。Layer 7はLayer 5と6が両方activeのときだけ条件付きで有効になり、Layer 6上のマウスボタンと競合するkey position 19/20/21をFNの `N5` / `N6` / `KP_PLUS` に置き換える。他のLayer 7位置はtransparentなのでLayer 5/6の動作を通す。Layer 6の30/31はFN Layer 5と同じミュート/中クリック割当。なおProspector設定ではZMK Studioのlayer reorderingが有効で、実機で優先順を変更している場合はこの既定順と異なる可能性がある。今回の動作確認ではStudio上のレイヤー並べ替えを行わず、必要なら既定順へ戻して確認する。

Layer 5のkeymap処理とNape scroll入力は別経路で同時に動く。通常X/Yは `nape_motion_listener` のLayer 5 overrideで、慣性tracker → `zip_xy_to_scroll_mapper` → `zip_scroll_scaler 1/8` の順にUSB HID scrollへ送る。overrideに `process-next` は付けず、親側のLayer 6 700ms `zip_temp_layer` を呼ばない。ボタンと物理wheelは従来の別 `nape_controls_listener` を使う。

慣性trackerはLayer 5中のNape XYだけを観測し、通常のmapper/scaler経路を維持する。40ms入力停止後、速いフリックでのみ慣性を始め、system workqueue上のdelayable workを16ms周期で動かす。速度はQ8固定小数点EMA `(old×3 + sample)/4`、方向反転時は旧速度を捨て、tickごとに230/256へ減衰する。最低開始速度は8 raw count/tick、停止thresholdは1 count/tick、最大継続は1200ms。X/Yは別速度・端数を持ち、既存1/8 scalerを慣性scrollにも一度だけ適用する。数値は `nape_bridge/inertia_math.h` に集約。

慣性scrollは専用virtual input deviceから直接wheel/hwheelを出すため、通常のXY mapperやLayer 6 temporary-layer timerに入り直さない。入力スレッドへ遅れて届いた古い合成イベントは世代タグで破棄する。Layer 5がOFF、Napeが切断、または新しいXYが来たとき速度・端数を消し、保留中のイベントも無効化する。ZMK標準scroll mapperの符号は維持し、Xはhorizontal wheel、Yはvertical wheel。方向を逆にする場合は標準transform processorをmapperの前に加える。

Layer 5はFNキーのholdでもscroll modifierでもあるため、同じhold操作を使う。必要ならKeymap Editorで追加の任意キーへ `&mo 5` を割り当てられる。Layer 8/9はinactive legacy slotsなので新しい設定では使わない。

## Nape Proのペアリング

1. ProspectorのUSBをPCへ接続し、Cornix LeftとRightが両方接続されるまで待つ。
2. Nape Proの側面スイッチを `BT` へ切り替える。既にPCとペアリング済みのBluetoothチャンネルではなく、空いているチャンネルを選ぶ。
3. Nape Proの丸いFnボタンと `1` / `2` / `3` のいずれかを約4秒押してペアリングモードにする。青いLEDの点滅を確認する。[KeychronのNape Pro案内](https://www.keychron.co.th/blogs/tutorial/nape-pro-nape-pro-quick-start-guide)に基づく操作で、製品の版によって表記が違う場合は付属マニュアルを優先する。
4. Prospectorは広告名に `Nape Pro` を含むデバイスを探索し、接続後に暗号化・HID service discovery・Report Map read・Input Report購読を進める。PC側のBluetooth設定でNapeをペアリングしない。
5. ペアリングが済めばProspector側settingsにbondが保存される。Napeが見つからない時は10秒のscan窓を挟んで再探索するため、点滅中にすぐ反応しない場合はしばらく待つ。

## 初回テスト

1. 旧UF2のバックアップを確認してから、通常版 `cornix_prospector_nape_bridge_nosd.uf2` をProspectorへ**手動**で書く。Cornix左右は現状維持。
2. Napeの電源を切ったまま、Cornix左右の文字入力を確認する。Baseの無変換/Enterをタップして従来の入力を確認し、同じ位置をホールドしてFN_SCROLL動作を確認する。
3. Napeをペアリングし、Layer 5をOFFのときはボールでカーソルが動いてLayer 6 `NAPE_MOUSE` が700ms後に解除されることを確認する。
4. 無変換またはEnter側をホールドしてLayer 5 `FN_SCROLL`を有効にし、Nape X/Yが横/縦スクロールになりカーソルが止まること、FNの数字/Fキーが同時に使えることを確認する。
5. ボールをゆっくり動かした場合は余分な慣性なし、強く弾いた場合は最大約1.2秒以内で減速停止、Layer 5を離したら直ちに停止することを確認する。
6. Nape buttonsと物理wheelは従来どおり、Layer 5/6同時active中も位置19/20/21がFNのN5/N6/KP_PLUSになること、マウスクリックへ化けないことを確認する。
7. Nape切断中もCornix入力が継続すること、再接続後に古いスクロールが出ないことを確認する。

## Cornixが接続表示なのに入力しない時

今回の症状は、複数split peripheralと `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` の組合せで、BLE接続表示は出てもキー通知用position-stateのGATT購読が欠落するZMK [Issue #3156](https://github.com/zmkfirmware/zmk/issues/3156) の条件に一致する。根本修正案 [PR #3411](https://github.com/zmkfirmware/zmk/pull/3411) は調査時点で未mergeのため、診断artifactはbattery fetching/proxyを無効化して競合を回避する。Prospector画面のCornixバッテリー残量はこの版では更新されない。

1. Nape Proの電源を切る。
2. `cornix_prospector_nape_bridge_split_stability_nosd.uf2` をProspectorだけに手動で書く。Cornix左右には書かない。
3. Prospectorを起動し、Cornix Leftを接続して入力を試す。次にRightを接続して両側を試す。
4. 入力が戻ればsplit GATT discovery競合の可能性が高い。戻らなければこの原因と断定せず、RTT debug logで `Found position state characteristic` と `[SUBSCRIBED]` を確認する。

この診断版を追加した時点ではNape Proの実機動作は未検証だった。その後の2026-09-26のユーザー実機報告では、接続、PCカーソル、wheel/button、画面のレイヤー切替と約700ms後の復帰を確認している。今回のKeymap Editor対応後のUF2は、書き込みと実機再確認が必要。

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
- ポインタは動くがレイヤーが変わらない：Nape版buildが`config/cornix.keymap`を使い、ProspectorにNape版UF2を書いたか確認する。
- 入力が詰まる：`NAPE: input queue overflow`、RAM使用量、BLE接続の切断ログを確認する。queueは4要素で、overflow時は最新側を残す。
- Prospector画面でCornix左右が接続済みなのにキー入力できない：ZMKの複数split peripheralとbattery fetching併用時に、接続表示だけ先に出てposition-stateのGATT購読が欠ける既知の競合がある。診断版で回避する。通常版でのみ再発する場合はこの競合の可能性が高いが、実機ログでの確認は別途必要。

## 調整箇所と既知の制限

- layer indexと速度調整は `boards/shields/cornix_nape_bridge/nape_layer_index.h` の `NAPE_SCROLL_LAYER_INDEX`（5）、`NAPE_MOUSE_LAYER_INDEX`（6）、`NAPE_SCROLL_COMPAT_LAYER_INDEX`（7）、`NAPE_MOUSE_LAYER_TIMEOUT_MS`（700）および`NAPE_SCROLL_SCALER_NUMERATOR/DENOMINATOR`（初期値1/8）で管理する。CIは共通keymapの対応とレイヤー数10を検査する。
- 慣性の開始遅延、tick周期、decay、最低開始速度、stop threshold、最大時間は `nape_bridge/inertia_math.h` にまとめた。長く滑らせるにはdecay numeratorを大きく、早く止めるには小さくするかstop thresholdを上げる。発動しやすさはminimum start countsを下げ、開始判定待ちはstart delayで調整する。初期40msはReport周期実測前の値。
- `NAPE_MOUSE`の未割当キーはtransparent。既存のミュート、中クリック、エンコーダー設定は保持した。キー割当はKeymap Editorでfeature branchの`config/cornix.keymap`を開いて編集する。クリックはNape本体のボタンからも送る。
- parserは相対X/Y、wheel、水平wheel、8個までのButton fieldを扱う。ZMK USB mouseへ送るボタンは先頭5個。複雑なHID Report Map、64バイトを超える1通知、512バイトを超えるReport Map、Boot Mouseだけの機器は未対応。
- Nape実機のReport Map raw dumpは保存されていない。ユーザー実機でX/Y・wheel・button転送は動作確認済みだが、descriptorの内容や他機種への汎用性は未確認。未知のReportは通常版でUSBへ転送しない。
- [2026-09-28のCI](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36361974258)のリンク時RAMは通常版 `248570 / 262144` バイト（94.82%、残り13574バイト）、デバッグ版 `251514 / 262144` バイト（95.94%、残り10630バイト）。保守差分前の通常版 `256770` バイトから8200バイト減った。旧Prospectorのベースラインは `252730 / 262144` バイト（96.41%）。これは静的配置と設定済みスタックの値であり、BLE 3接続時の実際のスタック余裕や連続稼働は未測定。デバッグ版は短時間のRTT調査用とし、通常運用は通常版を使う。
- Bluetooth認証方式とレポート内容はNape本体の実機・ファーム版に依存する。実機で不適合が判明した場合は、HEX dumpを根拠に小型parserへ限定的に対応を追加する。

## 2026-09-27 保守差分の確認状況（レイヤー変更前）

- レイヤーindex検証はローカルで成功（`NAPE_MOUSE` index 10、timeout 700 ms）。workflowとmanifest 5ファイルのYAML parseも成功。
- [Nape HID parser run 36298536850](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36298536850) が成功。C11 `-Wall -Wextra -Werror` + ASan/UBSanでparserとqueue overflow/disconnect release testを実行し、layer index検証も通過。
- [Build ZMK firmware run 36298882703](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36298882703) が成功。通常matrixの12 build（Nape Prospector variants、Cornix Left/Right等）とbaseline Prospector build 1件、両archive mergeが成功した。
- runの `firmware` archiveは2.82 MB、GitHub表示のarchive SHA256は `197bb6e50bc715dcce636090cc59a1c1f07450a4ef6df0193074253afe757e6a`。Nape対応UF2はこのarchiveから取得する。`firmware-baseline-dev` archiveは369 KB、archive SHA256は `602bc626842b013a6787088ba2ea68d29311d43b4d6b5b58ff8e543c4884d6a5`。これらはZIP archiveのdigestで、個々のUF2 hashではない。
- このWindows環境にC toolchain / west / Zephyr SDKがないためローカルcompile/linkは未実行。Actionsはbuild成功したが、ログへの匿名アクセスが使えず、新しいリンク時RAM値と個別UF2のSHA256は未採取。目標の95%未満になったかは未確認。
- 切断処理はbutton releaseを通知キューに記録するよう補強した。切断workが新しいBLE接続後に実行された場合でも、古いheld-button状態を新接続へ持ち越さない。

## 2026-09-28 Keymap Editor対応の確認結果

- `NAPE_MOUSE`は共通 `config/cornix.keymap` の番号9。番号0〜8は保持し、Nape専用の追加レイヤーは廃止した。タイムアウトは700msのまま。ローカルのレイヤー検証とYAML parseは成功した。
- Keymap Editorで `feat/nape-pro-ble-bridge` ブランチを開き、番号9に `NAPE_MOUSE` が表示されることを確認した。未割当キーは `&trans`、リモート側で追加された左・中・右クリックも表示される。Editorで保存はしていない。
- [Nape HID parser run 36361570858](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36361570858) はparser testとlayer index検査が成功。[Build ZMK firmware run 36361974258](https://github.com/haneta007/cornix-lp-zmk-/actions/runs/36361974258) は通常matrixの12 build、切り戻しProspector、両archive mergeを含む全18 jobが成功した。
- `firmware` artifactの `cornix_prospector_nape_bridge_nosd.uf2` はSHA256 `298F4F7FC6490BC362512FA94B83BE60EF3DE2AD829190F1CD010E453AE34ABA`。Prospectorへ手動で書くのはこのUF2。`firmware-baseline-dev` の `cornix_prospector_dongle_nosd.uf2` はSHA256 `C43C7E0717F25A14D604F4BF64E92BAF18C6D280F0176BBECC8C63E160A472AC` で、保存済み旧UF2と一致した。Cornix左右もbuild成功し、今回のための再書き込みは不要。
- Keymap Editor対応後の新UF2による実機確認は未実施。書き込み後にCornix左右の文字入力、Napeのポインタ・wheel・button、番号9への切替と約700ms後の復帰を確認する。
