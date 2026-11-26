// SD カードのマウントと、録音ファイルへの書き込みタスク。
// audio タスクは open / write / close を投げるだけで、実際の書き込みは writer タスクが行う。
#pragma once

#include <cstddef>
#include <cstdint>

namespace storage {

constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kRecDir = "/sdcard/REC";
constexpr const char* kLogDir = "/sdcard/LOG";

// 満杯になる手前で録音を止めるために残しておく空き（MB）。16kHz で約 4 分。
// 空きを測る間（30 秒）に書く量と、書き込みの列にたまる量（最大 2MB）より大きくし、止めたというログも書けるようにする
constexpr uint32_t kReserveMb = 8;

// SD の空き（予備を除く）で録れる分数。sample_rate が 0（音声なし）なら 16kHz として数える
uint32_t minutes_left(uint32_t sample_rate);

bool mount();
void start_writer_task();

// 以下は audio タスクから呼ぶ。キューが一杯なら false（データは捨て、数を数える）
// wav: WAV ファイル。書き込みを確定するたびに、頭の長さを書き直す
bool open_file(const char* path, bool wav);
bool write(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len);
bool close_file();

// ログ 1 行を SD の LOG/YYYYMMDD.txt に書き足す（「ログを SD に保存」がオンのときだけ。どのタスクから呼んでもよい）
void append_log(const char* line);
// ここまでに書き足したログを、5 秒ごとの確定を待たずに SD に確定する（録音を閉じたとき。止めてすぐ SD を抜いても残るように）
void flush_log();

// カードを FAT32 に初期化する（2GB 以下のカードは FAT16）。中身はすべて消える。FAT として読めないカード
// （exFAT など）も初期化できる。録音していないときに UI から呼ぶ。終わるまで待ち、成功したら true
bool format_card(uint32_t timeout_ms);

// 電源を切る前に呼ぶ。積まれた書き込みを終え、ファイルを閉じて SD を外す（終わるまで待つ）
bool shutdown(uint32_t timeout_ms);

}  // namespace storage
