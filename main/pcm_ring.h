// 1 書き手・1 読み手の PCM（16bit モノラル）リングバッファ（PSRAM に確保）。
// 録音用: 書き手は Bluetooth の受信コールバックか内蔵マイクのタスク、読み手は audio タスク。
// ループバック用: 書き手は受信コールバック、読み手は送信コールバック。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class PcmRing {
public:
    // overflow_counter を渡すと、溢れて捨てた回数を数える。
    // marks を true にすると、サンプルごとに 1 ビットの印を付けられる（録音用で、補間で作った音の印）
    bool init(size_t capacity_samples, std::atomic<uint32_t>* overflow_counter = nullptr, bool marks = false);

    // data の番地が 2 の倍数でなくてもよい（Bluetooth の受信データは奇数番地のことがある）
    // 書けなかった分は捨てる（戻り値は書けたサンプル数）。mark は、書いたサンプルに付ける印
    size_t write(const void* data, size_t n_samples, bool mark = false);
    // marked を渡すと、読んだうちの印の付いたサンプル数を返す
    size_t read(void* out, size_t n_samples, size_t* marked = nullptr);
    size_t available() const;
    // 読み手だけが呼ぶ（溜まっている分を捨てる）
    void discard_all();

private:
    void set_marks(size_t pos, size_t n, bool on);
    size_t count_marks(size_t pos, size_t n) const;

    int16_t* buf_ = nullptr;
    // 印（1 サンプル 1 ビット）。書き手は空いている側のビットだけを変え（同じバイトのほかのビットはそのまま書き戻す）、
    // 読み手は読むだけなので、同じバイトを両方が触っても読み手の分は変わらない
    uint8_t* marks_ = nullptr;
    size_t cap_ = 0;
    std::atomic<uint32_t>* overflows_ = nullptr;
    std::atomic<size_t> head_{0};  // 書き手が進める（累計）
    std::atomic<size_t> tail_{0};  // 読み手が進める（累計）
};

extern PcmRing g_pcm_ring;

// 音源がサンプルレートを変える（音声接続が開いた・閉じた、音源を切り替えた）ときに呼ぶ。
// audio タスクがこれを見て、溜まった古いデータを捨ててエンコーダーを作り直す。
void pcm_source_changed(uint32_t sample_rate);
