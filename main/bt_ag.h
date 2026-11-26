// Bluetooth HFP の Audio Gateway（スマホ役）。
// インカムはヘッドセット役（HF）として、Core2 からの接続を待つ（B+COM なら B2 長押しの待機状態）。
// 受け取る音声は「ヘッドセットのマイク音声」= インカムがミックスした会話。送る音声は無音。
#pragma once

#include <cstddef>
#include <cstdint>

namespace bt_ag {

struct FoundDevice {
    uint8_t bda[6];
    char name[40];
    int8_t rssi;
    uint32_t cod;
    bool is_audio;  // 機器の種類がオーディオ（ヘッドセットなど）
};

void init();

void start_discovery();
void stop_discovery();
int found_devices(FoundDevice* out, int max);  // 見つかった機器（オーディオ機器が先）

// Core2 が鍵を持っている（ペアリング済みの）機器
struct PairedDevice {
    uint8_t bda[6];
    char name[40];  // 名前の控えがなければアドレス
    bool last;      // 最後に接続した機器
    bool connected; // いまつながっている
};
int paired_devices(PairedDevice* out, int max);
bool is_paired(const uint8_t bda[6]);
// ペアリング済みの機器につなぐ（最後に接続した機器にする）
void connect_paired(const uint8_t bda[6], const char* name);
// ペアリングを削除する（つながっていれば切る。最後に接続した機器なら、それも忘れる）
void remove_pairing(const uint8_t bda[6]);

// 接続先を選んで接続する（選んだ機器は NVS に保存）
void connect_to(const uint8_t bda[6], const char* name);
// 保存済みの接続先に接続する。保存がなければ false
bool connect_saved();
// 切る。録音中のつなぎ直しもやめる
void disconnect();
// 録音中に切れて、最後に接続した機器へのつなぎ直しを続けている（つながる・録音を止める・切断するまで）
bool reconnecting();
// 接続での Core2 の役割（0 = 親機、1 = 子機、-1 = つながっていない）。調べるとき用
int link_role();
// 最後に接続した機器のアドレス。なければ false。調べるとき用
bool peer_address(uint8_t out[6]);

// 診断用: 音声接続だけを開く・閉じる。閉じたら、開く（audio_open か、インカムのボタンで頼まれる）まで、
// つなぎ直しても自動では開かず、開き直しもしない
void audio_open();
void audio_close();
// 音声接続を開いておきたいか（audio_close で閉じたら false。電源を入れ直すと true）
bool audio_wanted();
// 開閉を頼んで、まだ完了の知らせが来ていない（最大 3 秒）
bool audio_busy();
// 音声接続を閉じて開き直す（B2 を押して閉じ、また開いたときと同じ）。開けなければ、開けるか SLC が切れるまで
// 試し続ける。頼めたら true（音声接続が開いていない・閉じておきたい・開閉の途中・開き直しの途中なら false）
bool audio_reopen();
// 最後に開いた音声接続が、audio_reopen で開いたものか（false なら、B2・つなぎ直し・メニューなどで開いた）
bool last_open_was_reopen();

// ループバック: 受け取った声を少し遅らせてインカムへ返す（自分の声の届き方を聞くため）。
// 音声接続が切れると自動でオフになる
void set_loopback(bool on);
constexpr uint32_t kLoopbackDelayMs = 500;

// インカムのマイク感度（HFP の microphone gain、0〜15）を指示して保存する。次に接続したときも同じ値を指示する。
// インカムが遠隔設定に対応していなければ、ここでも接続時も何も送らない
void set_mic_gain(int gain);

}  // namespace bt_ag
