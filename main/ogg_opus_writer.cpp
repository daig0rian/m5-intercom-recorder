#include "ogg_opus_writer.h"

#include <cstring>

namespace {

uint32_t s_crc_table[256];
bool s_crc_ready = false;

void init_crc() {
    // Ogg の CRC32: 多項式 0x04C11DB7、反転なし、初期値 0
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        s_crc_table[i] = r;
    }
    s_crc_ready = true;
}

uint32_t crc_update(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ s_crc_table[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

void put_le16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}
void put_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}
void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

// パケット長を Ogg の lacing 値に分ける。戻り値は使ったセグメント数
int lace(size_t len, uint8_t* segs) {
    int n = 0;
    while (len >= 255) {
        segs[n++] = 255;
        len -= 255;
    }
    segs[n++] = static_cast<uint8_t>(len);
    return n;
}

constexpr uint8_t kFlagBos = 0x02;
constexpr uint8_t kFlagEos = 0x04;

}  // namespace

void OggOpusWriter::begin(uint32_t serial, uint32_t input_sample_rate, uint16_t pre_skip, const char* vendor,
                          const char* date_comment, PageSink sink) {
    if (!s_crc_ready) init_crc();
    sink_ = std::move(sink);
    serial_ = serial;
    seq_ = 0;
    granule_ = 0;
    nsegs_ = 0;
    body_len_ = 0;
    packets_ = 0;

    // OpusHead（RFC 7845 5.1）
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;   // version
    head[9] = 1;   // channels（モノラル）
    put_le16(head + 10, pre_skip);
    put_le32(head + 12, input_sample_rate);
    put_le16(head + 16, 0);  // output gain
    head[18] = 0;            // channel mapping family
    uint8_t segs[4];
    int n = lace(sizeof(head), segs);
    emit_page(segs, n, head, sizeof(head), kFlagBos, 0);

    // OpusTags（RFC 7845 5.2）
    uint8_t tags[256];
    size_t pos = 0;
    size_t vlen = strlen(vendor);
    size_t clen = date_comment ? strlen(date_comment) : 0;
    if (vlen > 64) vlen = 64;
    if (clen > 128) clen = 128;
    memcpy(tags, "OpusTags", 8);
    pos = 8;
    put_le32(tags + pos, vlen);
    pos += 4;
    memcpy(tags + pos, vendor, vlen);
    pos += vlen;
    if (clen > 0) {
        put_le32(tags + pos, 1);
        pos += 4;
        put_le32(tags + pos, clen);
        pos += 4;
        memcpy(tags + pos, date_comment, clen);
        pos += clen;
    } else {
        put_le32(tags + pos, 0);
        pos += 4;
    }
    n = lace(pos, segs);
    emit_page(segs, n, tags, pos, 0, 0);
}

void OggOpusWriter::add_packet(const uint8_t* packet, size_t len, uint32_t samples_48k) {
    uint8_t segs[kMaxSegments];
    int n = lace(len, segs);
    // いっぱいのページは、次のパケットが来たときに書き出す。最後のページにパケットが残り、
    // 終わりの印がパケットの入ったページに付く（空のページの granule は -1 にする決まりがあるため）
    if (packets_ >= kPacketsPerPage || nsegs_ + n > kMaxSegments || body_len_ + len > kMaxBody) flush(false);
    if (n > kMaxSegments || len > kMaxBody) return;  // 20ms の音声パケットではありえない大きさ

    memcpy(segs_ + nsegs_, segs, n);
    nsegs_ += n;
    memcpy(body_ + body_len_, packet, len);
    body_len_ += len;
    packets_++;
    granule_ += samples_48k;
}

void OggOpusWriter::flush(bool eos) {
    if (nsegs_ == 0 && !eos) return;
    // 音声を 1 つも書かずに閉じたときだけ、終わりの印だけの空のページになる（granule は -1）
    emit_page(segs_, nsegs_, body_, body_len_, eos ? kFlagEos : 0, nsegs_ == 0 ? UINT64_MAX : granule_);
    nsegs_ = 0;
    body_len_ = 0;
    packets_ = 0;
}

void OggOpusWriter::emit_page(const uint8_t* segs, int nsegs, const uint8_t* body, size_t body_len, uint8_t flags,
                              uint64_t granule) {
    uint8_t header[27 + kMaxSegments];
    memcpy(header, "OggS", 4);
    header[4] = 0;  // version
    header[5] = flags;
    put_le64(header + 6, granule);
    put_le32(header + 14, serial_);
    put_le32(header + 18, seq_++);
    put_le32(header + 22, 0);  // CRC（あとで入れる）
    header[26] = static_cast<uint8_t>(nsegs);
    memcpy(header + 27, segs, nsegs);
    size_t header_len = 27 + nsegs;

    uint32_t crc = crc_update(0, header, header_len);
    crc = crc_update(crc, body, body_len);
    put_le32(header + 22, crc);

    if (sink_) sink_(header, header_len, body, body_len);
}
