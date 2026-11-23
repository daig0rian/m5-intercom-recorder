// アプリ全体で共有する状態。各タスクが書き、UI タスクが読む。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

enum class BtState : uint8_t {
    Off,
    Idle,          // 待機（未接続）
    Discovering,   // 機器を検索中
    Connecting,    // SLC 接続中
    Connected,     // SLC 接続済み（音声は未接続）
    AudioOn,       // 音声接続済み
};

enum class Codec : uint8_t { None, Cvsd, Msbc };

constexpr int16_t kNoLevel = INT16_MIN;  // 前回 UI が読んでから新しいフレームがない

struct AppState {
    // Bluetooth
    std::atomic<BtState> bt_state{BtState::Off};
    std::atomic<Codec> codec{Codec::None};
    std::atomic<uint32_t> rx_bytes{0};       // 受信した PCM バイト数（累計）
    std::atomic<uint32_t> rx_rate_bps{0};    // 直近 1 秒の受信バイト数
    std::atomic<uint32_t> audio_sessions{0}; // 音声接続が開いた回数
    // 受信パケットの欠け（誤り + 受信なし + 一部欠け）の割合。直近 2 秒、% × 10（-1 = データなし）
    std::atomic<int16_t> pkt_drop_x10{-1};
    std::atomic<bool> loopback{false};       // 受信した声を遅らせてインカムへ返す
    std::atomic<int8_t> hf_mic_gain{-1};        // インカムのマイク感度 0〜15（-1 = 不明）
    std::atomic<bool> hf_remote_volume{false};  // インカムが感度の遠隔設定に対応している

    // 音声
    std::atomic<uint32_t> sample_rate{0};    // 0 = 音声なし
    std::atomic<uint32_t> source_gen{0};     // 音声接続が開閉する（サンプルレートが変わる）たびに増える
    std::atomic<uint32_t> ring_overflows{0}; // リングバッファ溢れ（データ欠け）の回数

    // レベル（AGC 前）。dBFS * 10 の整数で、UI が読むまでの最大値を持つ（読んだら kNoLevel に戻す）
    std::atomic<int16_t> level_rms_x10{kNoLevel};
    std::atomic<int16_t> level_peak_x10{kNoLevel};
    std::atomic<uint32_t> clip_frames{0};
    std::atomic<uint32_t> frames_processed{0};

    // 録音
    std::atomic<bool> rec_requested{false};  // UI が立てる
    std::atomic<bool> recording{false};      // 実際にファイルへ書いている
    std::atomic<uint32_t> rec_started_ms{0};
    std::atomic<uint32_t> rec_audio_ms{0};   // 録音できた音声の長さ（現ファイル）
    std::atomic<uint32_t> files_written{0};
    std::atomic<uint32_t> split_ms{30 * 60 * 1000};  // ファイルを分ける長さ（0 = 分けない）。メニューで変える
    std::atomic<bool> log_to_sd{true};  // ログを SD に保存する（メニューで変える）
    std::atomic<bool> reconnect_on_boot{true};  // 起動時に最後に接続した機器へつなぎに行く（メニューで変える）

    // SD
    std::atomic<bool> sd_mounted{false};
    std::atomic<bool> sd_unreadable{false};  // カードは応えるが、FAT として読めない（exFAT など）
    std::atomic<uint32_t> sd_free_mb{0};
    std::atomic<uint32_t> written_kb{0};  // 録音ファイルに書いた量（KB）
    std::atomic<uint32_t> write_errors{0};
    std::atomic<uint32_t> write_queue_drops{0};
};

extern AppState g_state;

// 文字列の状態（ファイル名・接続先名など）は短いので mutex で守ったコピーを使う
void state_set_peer_name(const char* name);
void state_get_peer_name(char* out, size_t len);
void state_set_file_name(const char* name);
void state_get_file_name(char* out, size_t len);

// 画面に出す直近のイベント（Bluetooth の動きの確認用）
void event_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
constexpr int kEventLineLen = 96;
constexpr int kEventLogLines = 500;  // 画面のログ画面で読める行数（PSRAM に持つ）
// 起動の最初に呼ぶ。sink には、1 行ごとに渡す先（SD への保存など）を指定する
void event_log_init(void (*sink)(const char* line));
int event_log_snapshot(char lines[][kEventLineLen], int max_lines);  // 最新の max_lines 行（新しいものが後ろ）
int event_log_read(int start, int n, char lines[][kEventLineLen]);  // 古い方から start 行目から n 行
int event_log_count();                                              // いま持っている行数
uint32_t event_log_total();                                         // これまでに書いた行数（新着の判定用）

uint32_t millis32();
