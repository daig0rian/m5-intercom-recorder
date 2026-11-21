// Opus パケットを Ogg ページに詰める（RFC 7845 / RFC 3533）。
// 書き出し先はコールバックで受け取る（1 回の呼び出し = Ogg ページ 1 枚。ヘッダーと本体に分けて渡す）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

class OggOpusWriter {
public:
    using PageSink = std::function<void(const uint8_t* header, size_t header_len, const uint8_t* body, size_t body_len)>;

    // OpusHead と OpusTags のページを出す
    void begin(uint32_t serial, uint32_t input_sample_rate, uint16_t pre_skip, const char* vendor,
               const char* date_comment, PageSink sink);
    // samples_48k: このパケットの長さ（48kHz 換算。20ms なら 960）
    void add_packet(const uint8_t* packet, size_t len, uint32_t samples_48k);
    // 溜まっているパケットをページとして出す。eos=true で最後のページにする
    void flush(bool eos);

    uint64_t granule() const { return granule_; }

    // 1 ページに入れるパケット数の上限（20ms × 50 = 約 1 秒ごとにページを出す）
    static constexpr int kPacketsPerPage = 50;

private:
    void emit_page(const uint8_t* segs, int nsegs, const uint8_t* body, size_t body_len, uint8_t flags,
                   uint64_t granule);

    PageSink sink_;
    uint32_t serial_ = 0;
    uint32_t seq_ = 0;
    uint64_t granule_ = 0;

    static constexpr int kMaxSegments = 255;
    static constexpr size_t kMaxBody = 8192;
    uint8_t segs_[kMaxSegments];
    int nsegs_ = 0;
    uint8_t body_[kMaxBody];
    size_t body_len_ = 0;
    int packets_ = 0;
};
