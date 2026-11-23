// WAV ファイル（RIFF、16 ビットの PCM、モノラル）の頭の 44 バイト。
// 音声の長さは頭に書くので、書いている途中でも読めるよう、書き込みを確定するたびに書き直す（storage.cpp）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace wav {

constexpr uint32_t kHeaderBytes = 44;

// data_bytes: 音声の部分の長さ（書き始めは 0。あとで patch_sizes で書き直す）
void make_header(uint8_t* out, uint32_t sample_rate, uint32_t data_bytes);

// 開いているファイル（fd）の頭の長さを、ファイル全体の長さ file_bytes に合わせて書き直し、末尾に戻る。
// FILE を使っているときは、先に fflush しておくこと
bool patch_sizes(int fd, uint32_t file_bytes);

}  // namespace wav
