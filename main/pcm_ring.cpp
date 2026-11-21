#include "pcm_ring.h"

#include <cstring>

#include "app_state.h"
#include "esp_heap_caps.h"

PcmRing g_pcm_ring;

bool PcmRing::init(size_t capacity_samples, std::atomic<uint32_t>* overflow_counter) {
    buf_ = static_cast<int16_t*>(heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buf_) {
        buf_ = static_cast<int16_t*>(heap_caps_malloc(capacity_samples * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    cap_ = buf_ ? capacity_samples : 0;
    overflows_ = overflow_counter;
    return buf_ != nullptr;
}

size_t PcmRing::write(const void* data, size_t n) {
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
    head_.store(head + todo, std::memory_order_release);
    return todo;
}

size_t PcmRing::read(void* out, size_t n) {
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
