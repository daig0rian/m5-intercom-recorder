# CLAUDE.md — m5-intercom-recorder

バイク用 Bluetooth インカム（主に B+COM）の会話を、M5Stack Core2 で受け取って microSD カードに録音する専用レコーダー。
このファイルは開発者向けに、目的と、調べて確かめた事実、まだ確かめていないこと、次にやることをまとめたもの。使い方は README.md。

## 作業ルール

- ドキュメント・画面の表示文言・コメントは**日本語**。
- 本書の「事実」は、公式資料・SDK のソース・実機で確かめたもの。
- ESP-IDF の API 名・Kconfig 名はバージョンで変わる。使うバージョンのヘッダ（`esp_hf_ag_api.h` など）とサンプル（`examples/bluetooth/bluedroid/classic_bt/hfp_ag`）で確かめる。
- リポジトリ名・製品名に他社の商標（B+COM、SENA、Cardo、GoPro、Insta360 など）を入れない。README に「B+COM 対応」と書く程度にとどめる。

## 目的

- 2 人のライダーが B+COM のインカム（B+LINK）で会話しながらツーリングしている。その会話を録音して、あとで Mitsuba EDR などの全時間記録のドライブレコーダーの映像と一緒に編集し、走行の動画に声を乗せたい。
- B+COM には、アクションカメラ（GoPro / Insta360）をユニバーサル接続でつなぎ、会話を映像に録音する公式機能がある。ただし次の不満がある。
  - 6 時間を超えるツーリングだと、カメラの電池や SD カードの交換が要る。カメラの熱の問題も出て、安定して収録できなかったり、収録できても画質が安定しなかったりする。
  - カメラとのペアリングがうまくいかないことがあり、原因が分からないことが多い。
  - カメラをペアリングしているだけで、ライダー間の会話に「カンカン」「コンコン」と何かを叩くようなノイズが乗ることがある。
  - ライダー間でマイクのゲインに差があっても、出発前にそれを目で見て確かめ・合わせる手段がない。特に 2 人だと、確かめようがない。
- そこで、カメラの代わりに M5Stack Core2 をつなぐ専用レコーダーを作る。

### 利用者が求めること

- 走行中にちらっと見て、あとどれくらい録れるかを正しく知りたい（余裕があると思ったら無かった、を避ける）。
- 走っている間に切れても、停まらずに録音が戻ってほしい。
- 帰ってから、ドラレコの映像に声を合わせて編集したい（録音の時間が実際の時刻とずれない）。
- 出発前に、二人の声の大きさの差を目で見て確かめ、合わせたい。

## 仕組み

### B+COM のアクションカメラ録音機能（公式）

- 対応機種と必要なソフトウェアバージョン：SB6XR V1.3〜 / SX1 V1.1〜 / ONE V3.5〜 / SB6X V5.7〜。
- 動作確認済みのカメラ：Insta360 X3 / X4、GoPro HERO12 / HERO13。
- 注意事項：
  - 推奨通話人数は最大 4 人。
  - ユニバーサル接続のため通話品質が下がる。
  - 「新・B+LINK」には対応しておらず、品質がさらに下がる可能性がある。
- **ペアリングの役割分担**：
  - B+COM 側：B2 ボタンを長押しして待機する。
  - カメラ側：Bluetooth の相手を検索し、表示された B+COM の名前をタップする。
  - 接続できると B+COM から「B+COM2接続」と音声が流れる（B2 のインカム枠に入る）。
- **ツーリング時の手順**：B+LINK で通話をつなぐ → カメラを B+COM に接続 → 録画開始 → 録音できているのを確かめてから出発。
- 通信を安定させるため、カメラを使う人はグループの最前列か最後尾を走ることが推奨されている。
- 出典：https://sygnhouse.jp/products/bcom/tips/actioncam/ 、https://sygnhouse.jp/2025/02/bcom-update-actioncam/
- 古い FAQ（https://sygnhouse.jp/faq/bcom-sound-recording/ ）には「外部録音機器への音声出力機能はない」とあるが、2025 年のアップデート以前の情報で、現在は古くなっている。

### ユニバーサル通話の 2 つの向き

- 「インターコール」：B+COM から呼び出す。B+COM がスマホ役になり、ユニバーサル機能を持たない相手のヘッドセットにつなぎに行く。
- 「レシーブ」：相手からの呼び出しを受ける。相手がスマホ役で、B+COM は待つ。**カメラ録音はこちら**。
- SB6XR のマニュアルでは、アプリの「ユニバーサルインターコール」設定は**基本オフ（初期設定）**にし、相手がユニバーサル接続機能を持たない場合だけオンにする、とされている。
- 再接続：通話が始まらない場合は「B+COM を再起動して B2 を押す」か「相手インカム側で登録済みインカムを呼び出す」（SB6XR マニュアル）。
- SB6XR の仕様：Bluetooth 5.0、対応プロファイルは HSP / HFP / A2DP / AVRCP。マニュアルに広帯域（mSBC）の記載はないが、実機ではコーデック交渉で mSBC に決まった。
- 7X / 7X EVO（B+FLEX 世代）も「ユニバーサルインターコール・レシーブ」を備えるが、アクションカメラの動作確認リストにはない。7X のマニュアル（https://sygnhouse.jp/pdf/7x_users_manual.pdf ）は未読。
- 出典：https://sygnhouse.jp/pdf/sb6xr_users_manual.pdf 、https://sygnhouse.jp/products/bcom/bcom-7x-evo/

### Bluetooth の役割

- **Core2 は HFP の Audio Gateway（AG＝スマホ役）**として動く。カメラと同じ立場。
- B+COM は HFP の Hands-Free（ヘッドセット役）として、B2 の待機状態でつながれるのを待つ。
- Core2 が受け取る音声は「ヘッドセットのマイク音声（上り方向）」で、中身は B+COM がミックスしたインカムの会話（二人分がミックス済みのモノラル 1 本）。
- 通話の模擬（RING / 応答）は要らない。ペアリング済みなら B+COM の B2 短押しで B+COM から接続してくる。SLC のあと Core2 から開いた音声接続を、B+COM はそのまま受け入れた。

### ESP-IDF 側

- ESP-IDF には HFP AG の実装とサンプル `hfp_ag` がある。
- 音声のデータパスは PCM（GPIO 出力）と HCI の 2 種類。音声をソフトで扱い、Opus で圧縮するには **HCI データパスが必須**。
- mSBC（16kHz 広帯域）は、HCI データパスで WBS を有効にし、相手も mSBC に対応している場合に選ばれる。PCM データパスでは CVSD（8kHz）のみ。
- 音声接続の状態イベントに `ESP_HF_AUDIO_STATE_CONNECTED`（CVSD）と `ESP_HF_AUDIO_STATE_CONNECTED_MSBC` がある。
- 既知の問題：IDF v4.3〜4.4 の AG を HCI データパス＋WBS で使うと、送信方向の SCO でデータ欠けやプチノイズが出るという報告がある（https://esp32.com/viewtopic.php?p=102166 ）。
- API 参照：https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_hf_ag.html

## やらないこと（現時点）

- 話者の自動分離、ステレオ録音、A2DP の音楽の録音。
- 走行中の複雑な操作（安全のため、走行中の操作は録音の開始・停止程度にする）。
- B+COM 以外のインカムの正式サポート（動作確認の対象外）。

## 開発環境

- ESP-IDF v5.5.6。Windows なら EIM（`winget install Espressif.EIM-CLI`）で入れられる。
- 書き込みが「Write timeout」（esptool の Connecting の段階）で止まることがある。USB ケーブルを差し直すと直る。

## ソースの構成（main/）

| ファイル | 役割 |
|---|---|
| `main.cpp` | 起動。各タスクを立ち上げる |
| `app_state.cpp` | タスク間で共有する状態と、画面・SD に出すイベントのログ（`event_log`） |
| `bt_ag.cpp` | HFP AG。検索・ペアリング・SLC・音声接続、AT コマンドへの応答、受信 PCM をリングバッファへ、送信 |
| `bt_sc_guard.c` | Bluedroid の SC の格下げの判定を、Core2 が SC を使っていないときは効かせない（リンク時に差し替え） |
| `pcm_ring.cpp` | 受信 → audio タスクと、ループバック用の PCM リングバッファ（PSRAM） |
| `audio_pipeline.cpp` | 20ms フレーム化 → レベル計測 → Opus → Ogg。途切れた間の無音 |
| `ogg_opus_writer.cpp` | Ogg ページの組み立て（RFC 7845） |
| `storage.cpp` | SD のマウントと書き込みタスク。ログの SD への保存 |
| `level_meter.cpp` | フレームの RMS / ピーク |
| `settings.cpp` | 電源を切っても残す設定（NVS） |
| `rtc_clock.cpp` | RTC と本体の時刻、`settime.txt` での時刻合わせ |
| `sound.cpp` | 操作音と映像同期の音 |
| `spi_lock.cpp` | 液晶と SD の SPI バスの排他 |
| `ui.cpp` | 画面とタッチ操作、電池の残量と自動シャットダウン、映像同期の画面 |

## 作ってあるが、実機で確かめていない機能

確かめたら、この欄から消す。いまはない。

## 確認した事実（IDF v5.5.6 のソースと実機）

- 受信音声は「内部コーデック（`BT_HFP_USE_EXTERNAL_CODEC=n`、既定）＋旧 API `esp_hf_ag_register_data_callback`」で受ける。CVSD は 8kHz、mSBC は復号と欠損補間（PLC）を済ませた 16kHz の PCM で届く。
  - 新 API `esp_hf_ag_register_audio_data_callback` は外部コーデック用で、mSBC は未復号のフレームのまま届く。Kconfig には「内部コーデックは将来削除予定」とある。IDF を上げるときは確認すること。
- 送信の outgoing コールバックは、データを返す限りスタックから呼ばれ続ける。そのため時間に見合った量だけ返す（`bt_ag.cpp` の `tx_timer_cb`）。
- 音声接続が閉じたあとに `esp_hf_ag_outgoing_data_ready()` を呼ぶと、スタックは 1 回ごとにエラーログを出す（`invalid air mode: 255`）。その書き出し（約 5ms）が合図の間隔（5ms）より長いため、処理がたまり続けて BTU タスクが張り付き、Bluetooth 全体が止まる。閉じる・切断を頼んだ時点と、スタックが送信データを取りに来なくなった時点で、合図を止めている。
- 送信（Core2 → インカム）は、コントローラーの空き枠が返ってくる（Number of Completed Packets）たびに、Bluedroid の送信待ちの列から渡される。Bluetooth のタスク（コア 0）が遅れると無線で送るものが尽き、音が途切れる。遅れた分は列に残り続け、30 個（約 225ms）であふれて捨てられる（`SCO xmit Q overflow, pkt dropped`）。送受信の処理は、ふだんから最大 30ms ほどまとめて動いていた。画面全体の描き直し（PSRAM の画面データ 15 万バイト）を 5 回/秒にしていたときは、メニューで 4〜6 個/秒捨てられ、ループバックで周期的に「ぷつぷつ」と聞こえた。描き直しを 1 回/秒（操作したときはすぐ）にすると 0.4 個/秒以下になった。描き直しを増やすときは注意。
- 音声接続中は `esp_hf_ag_pkt_stat_nums_get` で受信パケットの誤り・欠けを数えられる（音声接続を開いてからの累計）。
- 市販の HFP ヘッドセットでは、SLC のあとコーデック交渉で mSBC に決まり、16kHz の音声が届いた。手元の距離で受信パケットの約 0.5% が誤り・受信なしだった（PLC で補われる）。
- B+COM の B2 短押しでは、Bluetooth のつながりは残ったまま、音声接続だけが閉じる（`AT+BVRA=0` で閉じ、`AT+BVRA=1` で開き直す）。
- 途切れた間の無音は、音声だけ閉じた場合・B+COM の電源を切った場合・離れて切れた場合・途切れの間にファイルの区切りが来た場合のどれでも、長さと位置が実際の時刻と合った（1kHz の音を入れて確かめた）。離れていく間は 20〜60ms の音の抜けが何度も入ったが、詰められずに時間どおりに補われ、切れたと分かる直前まで音が届き続けた。7 分半（うち途切れ約 4 分 44 秒）の録音で、ずれはあっても 1.4 秒未満。
- 同じヘッドセットから録った Ogg Opus（87 秒）は、録音開始時刻＋長さが閉じた時刻と 1 秒未満で一致し、話の途中の欠けもなかった。4〜8kHz の成分も入っていた（広帯域）。平均ビットレートは 10〜13kbps（VBR）で、1 時間で約 6MB。
- インカムが遠隔での音量・感度設定に対応しているかは、SLC 接続時の HF 機能フラグの 0x10（`BTA_AG_PEER_FEAT_VOL`）で分かる。B+COM（SB6XR V1.3）の機能フラグは 0x289（エコー・雑音抑制、音声認識、コーデック交渉、eSCO S4）で、0x10 がない。実際に感度を 0〜15 に変えても音量は変わらなかった。
- 市販の HFP ヘッドセットでは、感度 0 でミュートになり（「ミュート」の音声案内が流れる）、1 以上に戻すと解除された。
- 認証が HCI エラー 0x05（Authentication Failure）で失敗すると、スタックが Core2 側のペアリング情報を消す。相手側に古い情報が残っていると「BT接続」では直らないので、「機器を探す」から選び直したときは古いペアリング情報を消してから新しくペアリングする（`bt_ag::connect_to`）。
  - 保存したペアリング情報で、再起動後も「BT接続」からペアリングし直さずに再接続できた。
- Bluedroid は、鍵ができたときに相手が名乗った「SC（Secure Connections）対応」を保存し（再起動後も残る）、次の接続で相手が「非対応」と名乗ると格下げの攻撃を疑って、鍵を消して切る（`btm_sec_set_peer_sec_caps`、`Remote Device downgraded security from SC, deleting Link Key`）。自分が SC を使っているかも、鍵の種別も見ない。Core2 は SC を使わない（`sc_en = false`、IDF の初期値）ので鍵はいつも古い方式（鍵種別 4）で、この判定は何も守らない。
  - 市販のヘッドセットで、接続ごとに名乗りが変わるものがあった（ペアリング時は対応、Core2 からつないだときは非対応）。切ってつなぎ直すたびに鍵が消えていた。
  - そのため、Core2 が SC を使っていないときはこの判定を効かせない（`bt_sc_guard.c`。IDF は書き換えず、`main/CMakeLists.txt` の `--wrap` でリンク時に差し替える）。働いたときはシリアルに `bt_sc_guard` の警告が出る。この対策で、切ってからのつなぎ直しと、電源を入れ直したあとの再接続が通った。
  - Bluedroid の内部の構造体を使うので、IDF を上げたら、ビルドが通ることと、`btm_sec_set_peer_sec_caps` がまだ別のファイル（`btm_acl.c`）から呼ばれていること（同じファイルの中からの呼び出しは差し替わらない）を確かめる。
- 同じヘッドセットがスマホにもつながっているとき、Core2 からのペアリングの試みは途中で止まって失敗し（5〜30 秒）、相手からのつなぎ直しでペアリングできた（選んでから 20〜45 秒）。原因は未確認。
- 最後に接続した機器（このプロジェクトが NVS の `recorder` に保存）と、その鍵（Bluedroid が NVS の `bt_config.conf` に保存）は、保存している係が別。利用者には「Core2 が相手と鍵の両方を覚えている」ように見えるので、「最後に接続した機器を覚えている ⇔ 鍵もある」を保つ。鍵を持っていたのに認証に失敗して鍵が消えたとき（相手側に鍵が無い）と、起動時に鍵が見つからないときは、最後に接続した機器も忘れて、ログで知らせる。相手が応答しなかっただけ（電源が切れているなど）のときは鍵が消えないので忘れない。
- Core2 の液晶は SPI3_HOST で、SD（sdspi、CS=4）と同じバス。M5GFX は IDF の SPI バスロック（`spi_device_acquire_bus`）を使うので共存できる。念のため描画と SD 書き込みは共通の mutex（`spi_lock`）でも排他にしている。
- IDF v5.5.6 の FatFs は exFAT が無効（`FF_FS_EXFAT 0`）。SD カードは FAT32 に限る。
- Core2 の SD スロットにはカード検出の信号がない。抜かれたことは、書き込みタスクが `sdmmc_get_status` で問い合わせて知る（`storage.cpp`）。
- 電源ボタンは電源 IC（AXP192 / AXP2101）につながる。M5Unified の初期設定では、長押しの知らせが押し始めから 1 秒、電源 IC による強制オフが 4 秒（AXP192 は 0x36=0x0C、AXP2101 は 0x27=0x00）。長押しの知らせを受けてから、録音を閉じて電源を切るまでは、実測で 40ms ほど。
- M5Unified のスピーカーのタスクは、既定では優先度 2・コア指定なし。これだと録音処理（優先度 10）や画面の描画に押されて操作音が途切れたり最初の音が抜けたりしたので、優先度 12・コア 1 にしている（`sound.cpp`）。
- 電源 IC は電源ボタンが押されたことを覚えているので、消灯中に `M5.Power.getKeyState()` で 50ms ごとに問い合わせても押し逃さない。
- 電池
  - 電源 IC は版によって違う（初期型・v1.3 は AXP192、v1.1 は AXP2101）。内蔵電池は初期型が 390mAh（M5Stack の変更履歴では 2023 年 10 月に 500mAh へ変わった）。
  - USB の有無は、AXP192 では ACIN（`Axp192.isACIN()`）で見る。AXP192 の Core2（初期型・v1.3）は USB が ACIN につながり、VBUS は 5V の端子（M-Bus・Grove）の側。電池で動いていても Core2 がそこへ 5V を出すので、VBUS は 5V 近くと読める（VBUS で見ていたときは、電池切れまで切れなかった）。AXP2101（v1.1）は VBUS で見る。
  - AXP192 は残量計を持たない。M5Unified は電池電圧から直線で残量を出す（`(mV - 3300) × 100 ÷ 800`）。リポ電池の電圧は終わりに急に下がるので、初期型・390mAh の録音中（消灯、130〜150mA、約 0.5W でほぼ一定）では、表示の 30% で残りは約 6 分（全体の 4%）しかなかった。そこで、この試験の電圧の下がり方から作った表（`ui.cpp` の `kAxp192Levels`）で換算している。表は劣化した 390mAh の 1 回分の測定から作ったもので、4.1V の点は外挿。
  - 同じ試験で、0% にした 3.5V から 3.3V まで約 4.4 分かかった。3.3V から電源 IC が切る 3.0V までは数十秒。3.3V で自分で切ったときも、録音ファイルを閉じてから止まった。
  - 画面を点けると電流が約 30mA（2 割）増え、電圧は 10mV ほど下がる（内部抵抗 約 0.3Ω）。
- 起動後の内部 RAM の空きは約 90KB（Bluetooth Classic と、audio タスクのスタック 40KB を含む）。
- **IRAM（命令用の内部メモリー、約 128KB）はほぼ満杯**（残り約 250 バイト）。中身はほとんど ESP-IDF とライブラリーで、PSRAM のキャッシュ対策のため、標準ライブラリーの文字列・ファイル入出力などの関数も既定で IRAM に置かれる。新しく使い始めた標準関数がそれに当たると IRAM が増える（例: `fputc` は `__swbuf_r` 137 バイトを呼び込んであふれたので `fwrite` にした）。あふれたらビルドが失敗する。空けるため、数学・乱数・環境変数の関数は IRAM から外している（`sdkconfig.defaults`）。使用量は build/m5-intercom-recorder.map の `_iram_text_end`（上限 0x400A0000）で見る。

## 次にやること

- B+LINK で二人がつながった状態で、二人の声が両方録れるか（U3）。
- カメラのときに乗る叩くようなノイズが、Core2 をつないだときにも乗るか。
- 長時間の録音試験（受信の安定性。U6）。
- 電池が切れるまでの録音試験（`ui.cpp` の `kBatteryTestLog` を true にすると、残量・電圧・放電電流がログに残る）を、満充電（充電の表示が消えてから）で始めて行い、`kAxp192Levels` と 0% の電圧を見直す。
  - Core2 初期型＋劣化した 390mAh（いまの表で、表示が均等に減るか）
  - Core2 初期型＋M5GO Bottom2（500mAh、LED 付き）
  - Core2 v1.3（AXP192、500mAh）。AXP192 には充放電の電流を積算する機能（クーロンカウンター）もある。電圧の表より合うかも確かめる。
  - README の電池の持ちは、いまは劣化した 390mAh の結果。v1.3 の結果で書き直す。
- AXP2101 の Core2（v1.1）で、残量の表示（電源 IC の残量計の値をそのまま使っている）と、0% での自動シャットダウンを確かめる。
- B+COM でも、シリアルに `bt_sc_guard` の警告（相手の SC の名乗りが変わった）が出ないかを見る。

## リリースの手順

1. `ui.cpp` の `kBatteryTestLog` が false であることを確かめる。
2. タグを付けてからビルドする（アプリのバージョンは git のタグから付く）。`git tag vX.Y.Z`、`idf.py build`、`idf.py merge-bin`。
3. GitHub のリリースに次を置く（`gh release create vX.Y.Z ...`）。
   - `m5-intercom-recorder-vX.Y.Z.bin`：`build/merged-binary.bin`。0x0 に書く。初めて書き込むとき用（設定とペアリングも消える）。
   - `m5-intercom-recorder-vX.Y.Z-update.bin`：`build/m5-intercom-recorder.bin`。0x10000 に書く。設定とペアリングを残して更新するとき用。パーティション表を変えたリリースでは置かない。
   - `THIRD_PARTY_LICENSES.txt`
4. 依存部品を変えたら、`THIRD_PARTY_LICENSES.txt` を見直す。

## 参考リンク

- B+COM アクションカメラ接続方法：https://sygnhouse.jp/products/bcom/tips/actioncam/
- B+COM アクションカメラ対応の告知（2025-02）：https://sygnhouse.jp/2025/02/bcom-update-actioncam/
- SB6XR ユーザーズマニュアル：https://sygnhouse.jp/pdf/sb6xr_users_manual.pdf
- B+COM 7X EVO 製品ページ：https://sygnhouse.jp/products/bcom/bcom-7x-evo/
- ESP-IDF HFP AG API：https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_hf_ag.html
- HFP AG＋HCI の既知の問題（フォーラム）：https://esp32.com/viewtopic.php?p=102166
- B+COM でのインカム録音の自作事例（ブログ）：https://doudeshou.blog.jp/archives/14627197.html
- BlueALSA の HFP/HSP 解説：https://github.com/arkq/bluez-alsa/wiki/Using-BlueALSA-with-HFP-and-HSP-Devices
