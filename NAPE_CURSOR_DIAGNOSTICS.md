# Napeカーソル飛びの切り分け

## 書き込むUF2

GitHub Actionsの firmware-cursor-check をダウンロードします。今回の比較用UF2だけが入っています。

1. 最初は cornix_prospector_nape_raw_cursor_nosd.uf2 をProspectorだけへ手動書き込み。
   - カーソル慣性OFF、ファームウェアのカーソル加速度OFF。
   - ZMK Studio、固定輝度80、通常のBLE接続、FN_SCROLL、スクロール横ブレ抑制・上下反転は維持。
2. ログが必要な場合は cornix_prospector_nape_raw_cursor_usb_log_nosd.uf2。
   - 同じrawカーソル動作にUSB CDC集計ログを追加。
   - この版ではCDCがログ用になるためStudio RPCは無効。保存済みStudio設定を意図的に消去しない。
   - 元の通常版を書き戻せばStudioを再び使用できます。

Cornix左右の書き込み・リセット・Nape bond削除は不要です。自動書き込みは行いません。
比較版でもWindowsのマウス設定やNape本体の出力特性までは無効にしません。

## 比較手順

- まずPCのUSBへ直結して、Napeをゆっくり動かしながら飛ぶか確認。
- USBポート、Napeとの距離、Windowsマウス設定を揃えて通常版／慣性のみOFF版／raw版を比較。
- raw版でも飛ぶ場合は、診断ログを30～60秒取得。最初の10秒は静止、その後ゆっくり動かし、飛んだ時刻も控える。
- キーボード入力、FNスクロール、Napeボタン・物理ホイールも確認。

## ログの読み方

NAPE: BLE interval_us=... latency=... timeout_ms=...
接続時の実際のBLEパラメータ。BLE updatedは接続後の変更。
例えばinterval_us=7500は7.5ms。接続要求の初期値は既存どおり30～50msで、短縮要求は追加しません。

NAPE: timing span_ms=... rx=... motion=... rx_gap_max_ms=... q_peak=... age_max_ms=... raw_max=... out_max=... overflow_total=... emit_errors=...

- span_ms: 集計時間。入力処理が動いたとき、前回から約1秒以上経過していれば出力。静止中は定期出力しない。
- rx: BLE Input Report通知の件数。button/wheel通知も含む。motionは処理された非ゼロXY reportの件数。
- rx_gap_max_ms: 通知間隔の最大値。意図的な静止・スリープも含むため、値だけで通信異常と判定しない。
- q_peak: 受信キュー件数のピーク。上限4。上限到達だけでは欠落の証明ではない。
- age_max_ms: 非ゼロXYを処理するまでの最大待ち時間。BLE callback内で時刻取得してから入力work内で変換後までの時間。
- raw_max / out_max: 各集計窓内のXY各軸の最大絶対値。FN_SCROLL OFFのraw版では一致するはず。同一reportのペア値をログに出しているわけではない。
- overflow_total: キュー溢れ累計。増加すると古いreportが捨てられた。再接続でもリセットしない。
- emit_errors: 当該集計窓内のvirtual inputへ相対イベントを入れる際の失敗件数。physical wheelも対象。USB送信成功そのものの確認値ではない。

集計stateは切断・接続時に初期化します。通常版では集計stateをコンパイルしません。
追加のthread、workqueue、周期work、大きなbuffer、BLE接続・設定変更はありません。
診断ログ自体が処理時間に影響する可能性があるため、raw通常版との比較も必要です。

## USBログを取得する

診断版を書き込むとProspectorのUSB CDCポートが現れます。WindowsのデバイスマネージャーでCOM番号を確認し、既存のシリアル端末があれば115200で開きます。
端末ソフトがない場合は、CodexでCOMポートを確認してログを取得できます。会社PCに新しい常駐アプリを導入する必要はありません。
ポートを開いた後にNapeを起こすと接続ログも取れます。接続時ログを取り逃した場合でも、timing集計は入力中に出ます。

## 切り戻し

cornix_prospector_nape_bridge_nosd.uf2 をProspectorへ手動で書き戻します。
通常の慣性あり・最大約1.4倍の加速度・Studioが戻ります。リセット不要です。

## 確認の限界

このログはBLE受信後からvirtual input注入までを測ります。電波再送回数、USB送信完了時刻、PCに届いた全HID reportを直接測るものではありません。
Nape本体が大きなXYを送っているのか、Bridge内で待っているのかをまず切り分けるための版です。
