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

// 診断用: 音声接続だけを開く・閉じる
void audio_open();
void audio_close();
// 開閉を頼んで、まだ完了の知らせが来ていない（最大 3 秒）
bool audio_busy();

// ループバック: 受け取った声を少し遅らせてインカムへ返す（自分の声の届き方を聞くため）。
// 音声接続が切れると自動でオフになる
void set_loopback(bool on);
constexpr uint32_t kLoopbackDelayMs = 500;

// インカムのマイク感度（HFP の microphone gain、0〜15）を指示して保存する。次に接続したときも同じ値を指示する。
// インカムが遠隔設定に対応していなければ、ここでも接続時も何も送らない
void set_mic_gain(int gain);

}  // namespace bt_ag
