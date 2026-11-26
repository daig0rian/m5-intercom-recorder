#include "pcm_ring.h"

#include <cstring>

#include "app_state.h"
#include "esp_heap_caps.h"

PcmRing g_pcm_ring;

namespace {
void* alloc_prefer_psram(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}
}  // namespace

bool PcmRing::init(size_t capacity_samples, std::atomic<uint32_t>* overflow_counter, bool marks) {
    buf_ = static_cast<int16_t*>(alloc_prefer_psram(capacity_samples * sizeof(int16_t)));
    if (buf_ && marks) {
        marks_ = static_cast<uint8_t*>(alloc_prefer_psram((capacity_samples + 7) / 8));
        if (!marks_) {
            heap_caps_free(buf_);
            buf_ = nullptr;
        } else {
            memset(marks_, 0, (capacity_samples + 7) / 8);
        }
    }
    cap_ = buf_ ? capacity_samples : 0;
    overflows_ = overflow_counter;
    return buf_ != nullptr;
}

void PcmRing::set_marks(size_t pos, size_t n, bool on) {
    for (size_t i = 0; i < n; i++, pos = pos + 1 == cap_ ? 0 : pos + 1) {
        uint8_t bit = static_cast<uint8_t>(1u << (pos & 7));
        if (on) marks_[pos >> 3] |= bit;
        else marks_[pos >> 3] &= static_cast<uint8_t>(~bit);
    }
}

size_t PcmRing::count_marks(size_t pos, size_t n) const {
    size_t c = 0;
    for (size_t i = 0; i < n; i++, pos = pos + 1 == cap_ ? 0 : pos + 1) c += (marks_[pos >> 3] >> (pos & 7)) & 1;
    return c;
}

size_t PcmRing::write(const void* data, size_t n, bool mark) {
    if (!buf_) return 0;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    size_t space = cap_ - (head - tail);
    size_t todo = n < space ? n : space;
    if (todo < n && overflows_) overflows_->fetch_add(1, std::memory_order_relaxed);

    size_t pos = head % cap_;
    size_t first = todo < cap_ - pos ? todo : cap_ - pos;
    memcpy(buf_ + pos, src, first * sizeof(int16_t));
    memcpy(buf_, src + first * sizeof(int16_t), (todo - first) * sizeof(int16_t));
    if (marks_) set_marks(pos, todo, mark);
    head_.store(head + todo, std::memory_order_release);
    return todo;
}

size_t PcmRing::read(void* out, size_t n, size_t* marked) {
    if (!buf_) return 0;
    uint8_t* dst = static_cast<uint8_t*>(out);
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);
    size_t avail = head - tail;
    size_t todo = n < avail ? n : avail;

    size_t pos = tail % cap_;
    size_t first = todo < cap_ - pos ? todo : cap_ - pos;
    memcpy(dst, buf_ + pos, first * sizeof(int16_t));
    memcpy(dst + first * sizeof(int16_t), buf_, (todo - first) * sizeof(int16_t));
    if (marked) *marked = marks_ ? count_marks(pos, todo) : 0;
    tail_.store(tail + todo, std::memory_order_release);
    return todo;
}

size_t PcmRing::available() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
}

void PcmRing::discard_all() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

void pcm_source_changed(uint32_t sample_rate) {
    g_state.sample_rate.store(sample_rate);
    g_state.source_gen.fetch_add(1);
}
