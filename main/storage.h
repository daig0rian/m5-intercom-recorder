// SD カードのマウントと、録音ファイルへの書き込みタスク。
// audio タスクは open / write / close を投げるだけで、実際の書き込みは writer タスクが行う。
#pragma once

#include <cstddef>
#include <cstdint>

namespace storage {

constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kRecDir = "/sdcard/REC";
constexpr const char* kLogDir = "/sdcard/LOG";

bool mount();
void start_writer_task();

// 以下は audio タスクから呼ぶ。キューが一杯なら false（データは捨て、数を数える）
bool open_file(const char* path);
bool write(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len);
bool close_file();

// ログ 1 行を SD の LOG/YYYYMMDD.txt に書き足す（「ログを SD に保存」がオンのときだけ。どのタスクから呼んでもよい）
void append_log(const char* line);
// ここまでに書き足したログを、5 秒ごとの確定を待たずに SD に確定する（録音を閉じたとき。止めてすぐ SD を抜いても残るように）
void flush_log();

// 電源を切る前に呼ぶ。積まれた書き込みを終え、ファイルを閉じて SD を外す（終わるまで待つ）
bool shutdown(uint32_t timeout_ms);

}  // namespace storage
