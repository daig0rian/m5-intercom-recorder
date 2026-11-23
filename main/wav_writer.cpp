#include "wav_writer.h"

#include <unistd.h>

#include <cstring>

namespace wav {
namespace {

void put_le16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

void put_le32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

bool write_le32_at(int fd, off_t offset, uint32_t v) {
    uint8_t b[4];
    put_le32(b, v);
    return lseek(fd, offset, SEEK_SET) == offset && write(fd, b, 4) == 4;
}

}  // namespace

void make_header(uint8_t* h, uint32_t sample_rate, uint32_t data_bytes) {
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBits = 16;
    constexpr uint16_t kBlockAlign = kChannels * kBits / 8;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, kHeaderBytes - 8 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);  // fmt の長さ
    put_le16(h + 20, 1);   // PCM
    put_le16(h + 22, kChannels);
    put_le32(h + 24, sample_rate);
    put_le32(h + 28, sample_rate * kBlockAlign);
    put_le16(h + 32, kBlockAlign);
    put_le16(h + 34, kBits);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

// IRAM がほぼ満杯で、新しく使い始めた標準関数で増えることがあるので、すでに使われている lseek・write で書き直す
bool patch_sizes(int fd, uint32_t file_bytes) {
    if (file_bytes < kHeaderBytes) return true;
    bool ok = write_le32_at(fd, 4, file_bytes - 8) && write_le32_at(fd, 40, file_bytes - kHeaderBytes);
    return lseek(fd, 0, SEEK_END) >= 0 && ok;
}

}  // namespace wav
