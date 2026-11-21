// 1 書き手・1 読み手の PCM（16bit モノラル）リングバッファ（PSRAM に確保）。
// 録音用: 書き手は Bluetooth の受信コールバックか内蔵マイクのタスク、読み手は audio タスク。
// ループバック用: 書き手は受信コールバック、読み手は送信コールバック。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class PcmRing {
public:
    // overflow_counter を渡すと、溢れて捨てた回数を数える
    bool init(size_t capacity_samples, std::atomic<uint32_t>* overflow_counter = nullptr);

    // data の番地が 2 の倍数でなくてもよい（Bluetooth の受信データは奇数番地のことがある）
    // 書けなかった分は捨てる（戻り値は書けたサンプル数）
    size_t write(const void* data, size_t n_samples);
    size_t read(void* out, size_t n_samples);
    size_t available() const;
    // 読み手だけが呼ぶ（溜まっている分を捨てる）
    void discard_all();

private:
    int16_t* buf_ = nullptr;
    size_t cap_ = 0;
    std::atomic<uint32_t>* overflows_ = nullptr;
    std::atomic<size_t> head_{0};  // 書き手が進める（累計）
    std::atomic<size_t> tail_{0};  // 読み手が進める（累計）
};

extern PcmRing g_pcm_ring;

// 音源がサンプルレートを変える（音声接続が開いた・閉じた、音源を切り替えた）ときに呼ぶ。
// audio タスクがこれを見て、溜まった古いデータを捨ててエンコーダーを作り直す。
void pcm_source_changed(uint32_t sample_rate);
