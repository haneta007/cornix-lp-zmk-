# Nape latency=0 比較版

## 最初に書き込むもの

firmware-latency-checkをダウンロードし、
cornix_prospector_nape_raw_cursor_latency0_usb_log_nosd.uf2
をProspectorだけへ手動書き込みます。Cornix左右の更新・リセット・bond削除は不要です。

この版は慣性とカーソル加速度をOFFにし、Napeだけの接続パラメータ変更要求をlatency=0へ調整します。
ZMKの大量デバッグ出力は抑え、NapeのBLEパラメータ・約1秒の入力集計を残します。
診断USBポートはログ用なのでStudio RPCは一時的にOFFです。

## 同梱した3版

- cornix_prospector_nape_raw_cursor_latency0_usb_log_nosd.uf2: latency=0方針＋USBログ。最初の確認用。
- cornix_prospector_nape_raw_cursor_latency0_nosd.uf2: 同じlatency方針、ログなし、Studioあり。普段使いの比較用。
- cornix_prospector_nape_raw_cursor_usb_log_nosd.uf2: latency方針は従来どおりで、余分なZMKログのみ抑制。A/B比較の対照版。

通常の慣性あり版・従来の慣性なし版・日常用セットは従来どおりです。
今回の比較版を通常版へ自動適用する変更はありません。

## なぜこの比較をするか

2026-10-03の実機ログでは、接続時50ms/latency=0から、接続後7.5ms/latency=30へ変わりました。
通知最大gapは繰り返し約240ms、受信後の処理待ちは0～1ms、受信ロック待ち0ms、キュー溢れ0、virtual input注入エラー0でした。
ただし、gapには静止も含まれ、USB送信完了を計測していないため、latencyが原因と確定したわけではありません。
ZMK debugログ3037行とログ欠落178件も確認し、ログ負荷を抑えた比較を用意しています。

## 固定APIと実装

ZMK 55d98b8adf8ac634c09486384fb207df1e98b5f2、
Zephyr 10ba6d0cb38bc3d258775d27982f707599320085は更新しません。
固定Zephyrのbt_conn_cb.le_param_reqは要求値の調整を許し、変更後の妥当性も検証します。
LL側の変更要求とL2CAP Connection Parameter Update Requestが共通のle_param_reqを通ることを確認しています。
現在のZMK BLE/split中央実装に競合するle_param_reqはありません。

CONFIG_ZMK_NAPE_ZERO_LATENCYは既定OFF。
有効時のcallbackはconn == bridge.connの場合だけparam.latencyを0にします。
interval_min/interval_max/timeoutには触れません。初回接続は従来どおりBT_LE_CONN_PARAM_DEFAULTで、latency=0です。
追加thread・workqueue・周期work・connection ref・BLE connectionはありません。
変更要求を調整する方式なので、実際の接続がlatency=0になったかをログで確認してください。
省電力のためのスキップを減らすことでNapeの電池消費が増える可能性があります。

## 実機確認

1. Prospectorへ診断版を手動書き込み。
2. COMポートを確認してログ取得を開始。COM番号は前回のCOM20から変わる場合があります。
3. Cornix左右を接続し、Napeを起こす。
4. NAPE: latency policy requested=30 accepted=0 のような行を確認。
5. NAPE: BLE updated ... latency=0 を確認。要求ログだけでは適用成功と判断しない。
6. 60～90秒ゆっくり動かして、飛んだ時刻を記録。
7. Cornix入力、FNスクロール、ボタン、物理wheelを確認。
8. 同条件で従来latencyの静かなログ版と比較。ログなしlatency=0版でも確認。

集計項目の詳細はNAPE_CURSOR_DIAGNOSTICS.mdを参照してください。
直結・ハブ接続、Napeとの距離、Windowsマウス設定を揃えて比較します。
latencyが0にならない、接続が不安定になる、飛びが残る場合はログを保存して次の調査へ進みます。

## 切り戻し

通常のcornix_prospector_nape_bridge_nosd.uf2をProspectorへ手動で書き戻します。
設定リセットは不要です。
