// 電源を切っても残す設定（NVS）。
#pragma once

#include <cstddef>
#include <cstdint>

#include "app_state.h"

namespace settings {

void init();  // nvs_flash_init を含む

// 接続先（初回ペアリングで選んだインカム）
bool get_peer(uint8_t bda[6], char* name, size_t name_len);
void set_peer(const uint8_t bda[6], const char* name);
void clear_peer();  // 最後に接続した機器を忘れる

// ペアリングした機器の名前の控え（Bluedroid の鍵の一覧にはアドレスしかないため、最大 8 台分を自分で持つ）
constexpr int kMaxNamedDevices = 8;
void set_device_name(const uint8_t bda[6], const char* name);
bool get_device_name(const uint8_t bda[6], char* name, size_t name_len);
void remove_device_name(const uint8_t bda[6]);

// インカムのマイク感度 0〜15。保存していなければ -1
int get_mic_gain();
void set_mic_gain(int gain);

// 録音ファイルを分ける長さ（秒）。0 = 分けない。保存していなければ 30 分
uint32_t get_split_sec();
void set_split_sec(uint32_t sec);

// ログを SD に保存するか。保存していなければ true
bool get_log_to_sd();
void set_log_to_sd(bool on);

// 起動時に、最後に接続した機器へつなぎに行くか。保存していなければ true
bool get_reconnect_on_boot();
void set_reconnect_on_boot(bool on);

// NVS をすべて消す（このプロジェクトの設定・Bluedroid のペアリングの鍵・M5GFX の機種の控え）。
// 消したあとは再起動すること
bool erase_all();

}  // namespace settings
